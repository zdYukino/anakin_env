from types import SimpleNamespace as NS

import pytest
from fastapi.testclient import TestClient
from gateway.protocol import Client, read_path, stamp_ns, subscriptions
from gateway.server import create_app


def test_paths_and_time():
    msg = NS(
        header=NS(stamp=NS(sec=7, nanosec=123)), joints=NS(position=[1.5, float("nan")])
    )
    assert read_path(msg, "joints.position[0]") == 1.5
    assert read_path(msg, "joints.position[1]") is None
    assert read_path(msg, "joints.position[99]") is None
    assert stamp_ns(msg) == 7_000_000_123


def test_bounded_queue_and_limits():
    c = Client(capacity=2)
    for n in range(4):
        c.push({"topic": "/a", "seq": n})
    packet = c.drain()
    assert [x["seq"] for x in packet["samples"]] == [2, 3]
    assert packet["dropped"] == 2
    assert c.drain()["samples"] == []
    with pytest.raises(ValueError):
        subscriptions([{"topic": "/a", "fields": [f"x[{i}]" for i in range(13)]}])


class FakeAdapter:
    def graph(self):
        return [{"name": "/test", "type": "std_msgs/msg/Float64"}]

    def configure(self, c, requests):
        c.active = requests
        c.push({"topic": "/test", "t": 1, "values": {"data": 2}})

    def schema(self, topic):
        return {
            "topic": topic,
            "type": "std_msgs/msg/Float64",
            "fields": [
                {"name": "data", "type": "double", "array": False, "children": []}
            ],
        }

    def detach(self, c):
        c.active = []


def test_read_only_websocket_contract():
    with TestClient(create_app(FakeAdapter())) as api:
        assert api.get("/api/health").json()["read_only"] is True
        with api.websocket_connect("/ws") as ws:
            assert ws.receive_json()["kind"] == "hello"
            ws.send_json({"v": 1, "kind": "publish", "topic": "/robot/control"})
            while (packet := ws.receive_json())["kind"] != "error":
                pass
            assert "read-only" in packet["message"]
            ws.send_json(
                {
                    "v": 1,
                    "kind": "subscribe",
                    "subscriptions": [{"topic": "/test", "fields": ["data"]}],
                }
            )
            while (packet := ws.receive_json())["kind"] != "batch":
                pass
            assert packet["samples"][0]["values"]["data"] == 2
