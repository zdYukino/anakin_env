import json
import time
import threading
from types import SimpleNamespace as NS

import pytest
from fastapi.testclient import TestClient

from gateway.control import control_request
from gateway.protocol import Client
from gateway.ros_adapter import Ros2Adapter
from gateway.server import create_app
from test_gateway import FakeAdapter


def frame(**fields):
    return {"v": 1, "kind": "control", "operation": "input", "axes": [0, 0, 0, 0], "buttons": [False] * 10, "shoulders": 0, **fields}


@pytest.mark.parametrize("changes", [
    {"axes": [0, 0, 0]}, {"axes": [True, 0, 0, 0]}, {"axes": [float("nan"), 0, 0, 0]},
    {"axes": [2, 0, 0, 0]}, {"buttons": [False] * 9}, {"buttons": [0] * 10},
    {"shoulders": True}, {"shoulders": 4}, {"session": "injected"}, {"topic": "/robot"},
    {"action": {}}, {"operation": "publish"},
])
def test_rejects_unbounded_or_untyped_control(changes):
    with pytest.raises(ValueError):
        control_request(frame(**changes))


def adapter_fixture(monkeypatch):
    adapter = Ros2Adapter.__new__(Ros2Adapter)
    adapter.call = lambda fn: fn()
    adapter.control_owner = None
    adapter.control_session = adapter.control_environment = ""
    adapter.control_sequence = 0
    adapter.control_last = 0
    adapter.control_activated = 0
    adapter.control_received = time.monotonic()
    adapter.control_runtime = {
        "schema_version": "1", "timeout_ms": "250", "enabled": "true", "environment": "env-session",
        "session": "", "active_source": "none", "mode": "real", "state": "disarmed",
        "requested_state": "disarmed", "moving": "false", "height": ".78", "error": "",
        "action.align": "true", "action.hold": "false", "action.emergency_stop": "true",
    }
    adapter.node = NS(get_publishers_info_by_topic=lambda _: [NS()])
    packets = []
    adapter.control_publisher = NS(publish=lambda message: packets.append(json.loads(message.data)))
    monkeypatch.setitem(__import__('sys').modules, "std_msgs.msg", NS(String=lambda: NS(data="")))
    return adapter, packets


def test_exclusive_lease_guard_revalidation_sequence_and_disconnect(monkeypatch):
    adapter, packets = adapter_fixture(monkeypatch)
    owner, other = Client(), Client()
    with pytest.raises(ValueError): adapter.control(owner, {"operation": "input", **{k: v for k, v in frame().items() if k not in ('v', 'kind')}})
    pending = adapter.control(owner, {"operation": "activate"})
    assert pending["pending"] and not pending["owned"]
    assert packets[-1]["axes"] == [0, 0, 0, 0]
    session = packets[-1]["session"]
    adapter.control_runtime["session"] = session
    assert adapter.control_status(owner)["owned"]
    assert not adapter.control_status(other)["owned"]
    with pytest.raises(ValueError): adapter.control(other, {"operation": "activate"})
    with pytest.raises(ValueError): adapter.control(owner, control_request(frame(action="hold")))
    adapter.control(owner, control_request(frame(action="align")))
    assert packets[-1]["sequence"] == 2 and packets[-1]["action"] == "align"
    adapter.release_control(other)
    assert adapter.control_status(owner)["owned"]
    adapter.control_last -= 1
    assert not adapter.control_status(owner)["owned"]
    assert packets[-1]["operation"] == "release"
    with pytest.raises(ValueError): adapter.control(owner, control_request(frame()))
    adapter.control_runtime["session"] = ""
    adapter.control(owner, {"operation": "activate"})
    assert packets[-1]["session"] != session
    adapter.control_runtime["session"] = packets[-1]["session"]
    adapter.release_control(owner)
    assert adapter.control_owner is None and packets[-1]["operation"] == "release"


def test_stale_runtime_or_multiple_publishers_cannot_activate(monkeypatch):
    adapter, packets = adapter_fixture(monkeypatch)
    client = Client()
    adapter.control_received -= 1
    with pytest.raises(ValueError): adapter.control(client, {"operation": "activate"})
    adapter.control_received = time.monotonic()
    adapter.node.get_publishers_info_by_topic = lambda _: [NS(), NS()]
    with pytest.raises(ValueError): adapter.control(client, {"operation": "activate"})
    assert packets == []


def test_missing_activation_ack_releases_without_renewing_the_lease(monkeypatch):
    adapter, packets = adapter_fixture(monkeypatch)
    client = Client()
    adapter.control(client, {"operation": "activate"})
    adapter.control_activated -= 1
    assert not adapter.control_status(client)["pending"]
    assert adapter.control_owner is None and packets[-1]["operation"] == "release"


def test_browser_origin_must_match_the_gateway():
    from starlette.websockets import WebSocketDisconnect
    with TestClient(create_app(FakeAdapter())) as api:
        with pytest.raises(WebSocketDisconnect):
            with api.websocket_connect("/ws", headers={"origin": "https://external.example"}):
                pass


def test_websocket_releases_owner_on_disconnect_and_rejects_generic_writes():
    class Controller(FakeAdapter):
        owner = None
        released = False
        release_event = threading.Event()
        def control_status(self, client):
            return {"available": True, "owned": self.owner is client}
        def control(self, client, request):
            self.owner = client
            return self.control_status(client)
        def release_control(self, client):
            if self.owner is client:
                self.owner = None
                self.released = True
                self.release_event.set()
    adapter = Controller()
    with TestClient(create_app(adapter)) as api:
        with api.websocket_connect("/ws") as ws:
            ws.send_json({"v": 1, "kind": "control", "operation": "activate"})
            for _ in range(20):
                packet = ws.receive_json()
                if packet["kind"] == "control_state" and packet.get("owned"): break
            assert packet.get("owned")
            ws.send_json({"v": 1, "kind": "service", "service": "/robot/start"})
            while (packet := ws.receive_json())["kind"] != "error": pass
            assert packet["code"] == "REQUEST_REJECTED"
        assert adapter.release_event.wait(timeout=1)
        assert adapter.released
