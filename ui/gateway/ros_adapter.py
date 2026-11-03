"""rclpy access lives exclusively in one executor thread."""

from __future__ import annotations

import concurrent.futures
import queue
import threading
import time
import json
import uuid
from collections import deque
from typing import Any, Callable

from .protocol import Client, read_path, safe_json, stamp_ns


class Ros2Adapter:
    def __init__(self):
        self.commands: queue.Queue = queue.Queue()
        self.ready = threading.Event()
        self.error: Exception | None = None
        self.running = True
        self.subs: dict[Client, list[Any]] = {}
        self.stats: dict[str, deque] = {}
        self.config_client = None
        self.config_node = ""
        self.config_lock = threading.Lock()
        self.config_checked = 0.0
        self.config_cache: dict = {}
        self.control_owner = None
        self.control_session = ""
        self.control_environment = ""
        self.control_sequence = 0
        self.control_last = 0.0
        self.control_activated = 0.0
        self.control_received = 0.0
        self.control_runtime: dict = {}
        self.thread = threading.Thread(
            target=self._run, name="ros-telemetry", daemon=True
        )
        self.thread.start()
        self.ready.wait(10)
        if self.error:
            raise RuntimeError(
                f"ROS2 unavailable: {self.error}. Source /opt/ros/humble/setup.bash and your message workspace."
            ) from self.error
        if not self.ready.is_set():
            self.running = False
            raise RuntimeError("ROS2 initialization timed out")

    def _run(self):
        context = node = executor = None
        try:
            import rclpy
            from rclpy.context import Context
            from rclpy.executors import SingleThreadedExecutor

            context = Context()
            rclpy.init(context=context)
            node = rclpy.create_node(
                "aerospace_telemetry_gateway", context=context, enable_rosout=False
            )
            self.node = node
            from diagnostic_msgs.msg import DiagnosticArray
            from std_msgs.msg import String
            from rclpy.qos import qos_profile_sensor_data, QoSProfile

            self.control_publisher = node.create_publisher(String, "/anakin/web_input", QoSProfile(depth=32))
            self.control_subscription = node.create_subscription(
                DiagnosticArray, "/anakin/input_state", self._control_feedback, qos_profile_sensor_data
            )
            executor = SingleThreadedExecutor(context=context)
            executor.add_node(node)
            self.ready.set()
            while self.running:
                while not self.commands.empty():
                    future, fn = self.commands.get_nowait()
                    if future.set_running_or_notify_cancel():
                        try:
                            future.set_result(fn())
                        except Exception as exc:
                            future.set_exception(exc)
                executor.spin_once(timeout_sec=0.004)
        except Exception as exc:
            self.error = exc
            self.ready.set()
        finally:
            if executor:
                executor.shutdown()
            if node:
                node.destroy_node()
            if context and context.ok():
                context.shutdown()

    def call(self, fn: Callable):
        if self.error or not self.thread.is_alive():
            raise RuntimeError(f"ROS executor stopped: {self.error}")
        future: concurrent.futures.Future = concurrent.futures.Future()
        self.commands.put((future, fn))
        return future.result(timeout=10)

    def graph(self) -> list[dict]:
        return self.call(self._graph)

    def _control_feedback(self, message):
        for status in message.status:
            if status.name != "anakin_env/web_control":
                continue
            values = {item.key: item.value for item in status.values}
            if values.get("schema_version") != "1":
                continue
            self.control_runtime = values
            self.control_received = time.monotonic()

    def _control_view(self, client):
        values = self.control_runtime
        timeout = min(1.0, max(0.05, int(values.get("timeout_ms", "250")) / 1000))
        available = bool(values) and time.monotonic() - self.control_received <= timeout
        if self.control_owner is not None and (
            not available or time.monotonic() - self.control_last > timeout
            or values.get("environment") != self.control_environment
            or (not values.get("session") and time.monotonic() - self.control_activated > timeout)
        ):
            self._release_control(self.control_owner)
        mine = self.control_owner is client
        owned = mine and available and values.get("session") == self.control_session
        pending = mine and available and not owned and not values.get("session")
        if mine and available and values.get("session") and not owned:
            self._release_control(client)
        return {
            "available": available,
            "timeout_ms": int(timeout * 1000),
            "sdk_publish_count": int(values.get("sdk_publish_count", "0")),
            "enabled": available and values.get("enabled") == "true",
            "owned": owned,
            "pending": pending,
            "leased": available and bool(values.get("session")),
            "environment": values.get("environment", "") if available else "",
            "active_source": values.get("active_source", "none") if available else "none",
            "mode": values.get("mode", "") if available else "",
            "state": values.get("state", "") if available else "",
            "requested_state": values.get("requested_state", "") if available else "",
            "moving": available and values.get("moving") == "true",
            "height": float(values.get("height", "0")) if available else 0,
            "actions": {key[7:]: value == "true" for key, value in values.items() if key.startswith("action.")} if available else {},
            "message": values.get("error", "") if available else "Waiting for live ENV control state",
        }

    def control_status(self, client):
        return self.call(lambda: self._control_view(client))

    def control(self, client, request):
        return self.call(lambda: self._control(client, request))

    def _control(self, client, request):
        from .control import control_request
        # Also validate callers outside the WebSocket handler.
        request = control_request({"v": 1, "kind": "control", **request})
        operation = request["operation"]
        if operation == "release":
            self._release_control(client)
            return self._control_view(client)
        view = self._control_view(client)
        if not view["available"] or not view["enabled"]:
            raise ValueError("Live ENV web control is unavailable")
        endpoints = self.node.get_publishers_info_by_topic("/anakin/input_state")
        if len(endpoints) != 1:
            raise ValueError("Expected one running ENV input publisher")
        if operation == "activate":
            if self.control_owner is not None or view["leased"]:
                raise ValueError("Another browser already holds web control")
            self.control_owner = client
            self.control_session = uuid.uuid4().hex
            self.control_environment = view["environment"]
            self.control_sequence = 0
            self.control_activated = time.monotonic()
        elif self.control_owner is not client or not (view["owned"] or view["pending"]):
            raise ValueError("Activate web control before sending input")
        elif "action" in request and (not view["owned"] or not view["actions"].get(request["action"], False)):
            raise ValueError("Action is not currently available")
        self.control_last = time.monotonic()
        self._publish_control(request)
        return self._control_view(client)

    def _publish_control(self, request):
        from std_msgs.msg import String
        self.control_sequence += 1
        packet = {
            "v": 1, "environment": self.control_environment,
            "session": self.control_session, "sequence": self.control_sequence,
            "sent_ms": time.time_ns() // 1_000_000,
            "axes": [0.0] * 4, "buttons": [False] * 10, "shoulders": 0,
            **request,
        }
        message = String()
        message.data = json.dumps(packet, allow_nan=False, separators=(",", ":"))
        self.control_publisher.publish(message)

    def _release_control(self, client):
        if self.control_owner is client:
            self._publish_control({"operation": "release"})
            self.control_owner = None
            self.control_session = ""

    def release_control(self, client):
        self.call(lambda: self._release_control(client))

    def input_config(self) -> dict:
        """Read only the unique input publisher's effective startup YAML.

        The ROS executor keeps spinning while the parameter response is pending.
        Never guess a local file, node name, or default controller mapping.
        """
        import yaml

        with self.config_lock:
            if time.monotonic() - self.config_checked < 0.75:
                return self.config_cache
            response: concurrent.futures.Future = concurrent.futures.Future()
            pending = None
            try:
                provider, client, pending = self.call(
                    lambda: self._request_input_config(response)
                )
                values = response.result(timeout=0.5).values
                if len(values) != 4 or [v.type for v in values] != [4, 4, 4, 2]:
                    raise ValueError("Runtime input configuration parameters unavailable")
                config = yaml.safe_load(values[0].string_value)
                if not isinstance(config, dict) or config.get("schema_version") != 2:
                    raise ValueError("Unsupported runtime input configuration")
                result = {
                    "available": True,
                    "node": provider,
                    "path": values[1].string_value,
                    "mode_path": values[2].string_value,
                    "input_timeout_ms": values[3].integer_value,
                    "config": config,
                }
            except (RuntimeError, ValueError, TimeoutError, yaml.YAMLError) as exc:
                result = {"available": False, "message": str(exc) or "Runtime configuration request timed out"}
            finally:
                if pending is not None and not pending.done():
                    self.call(lambda: client.remove_pending_request(pending))
            self.config_cache = result
            self.config_checked = time.monotonic()
            return result

    def _request_input_config(self, response):
        from rcl_interfaces.srv import GetParameters

        endpoints = self.node.get_publishers_info_by_topic("/anakin/input_state")
        if len(endpoints) != 1:
            raise RuntimeError("Expected one running env input publisher")
        endpoint = endpoints[0]
        provider = f"{endpoint.node_namespace.rstrip('/')}/{endpoint.node_name}"
        if provider != self.config_node:
            if self.config_client:
                self.node.destroy_client(self.config_client)
            self.config_node = provider
            self.config_client = self.node.create_client(
                GetParameters, f"{provider}/get_parameters"
            )
        client = self.config_client
        if not client.service_is_ready():
            raise RuntimeError("Runtime configuration service not ready")
        request = GetParameters.Request()
        request.names = ["input_config_yaml", "input_config_path", "mode_config_path", "input_timeout_ms"]
        pending = client.call_async(request)

        def complete(future):
            try:
                response.set_result(future.result())
            except Exception as exc:
                response.set_exception(exc)

        pending.add_done_callback(complete)
        return provider, client, pending

    def _graph(self):
        now = time.monotonic()
        graph = []
        for topic, types in self.node.get_topic_names_and_types():
            endpoints = self.node.get_publishers_info_by_topic(topic)
            stat = self.stats.get(topic, deque())
            while stat and now - stat[0][0] > 2:
                stat.popleft()
            duration = max(0.1, stat[-1][0] - stat[0][0]) if len(stat) > 1 else 1
            qos = [
                {
                    "reliability": p.qos_profile.reliability.name,
                    "durability": p.qos_profile.durability.name,
                    "depth": p.qos_profile.depth,
                    "node": p.node_name,
                }
                for p in endpoints
            ]
            graph.append(
                {
                    "name": topic,
                    "type": types[0],
                    "publishers": len(endpoints),
                    "subscribers": self.node.count_subscribers(topic),
                    "qos": qos,
                    "hz": (len(stat) - 1) / duration if len(stat) > 1 else 0,
                    "bytes_per_second": sum(x[1] for x in stat) / duration,
                    "last_receive_ns": str(stat[-1][2]) if stat else None,
                }
            )
        return graph

    def schema(self, topic: str) -> dict:
        return self.call(lambda: self._schema(topic))

    def _schema(self, topic):
        from rosidl_runtime_py.utilities import get_message
        from rosidl_parser.definition import AbstractNestedType, NamespacedType

        types = dict(self.node.get_topic_names_and_types()).get(topic)
        if not types:
            raise ValueError(f"Topic not discovered: {topic}")
        message_type = types[0]
        cls = get_message(message_type)

        def describe(msg_cls, depth=0):
            if depth > 12:
                return []
            result = []
            for (name, text), slot in zip(
                msg_cls.get_fields_and_field_types().items(), msg_cls.SLOT_TYPES
            ):
                nested = (
                    slot.value_type if isinstance(slot, AbstractNestedType) else slot
                )
                children = []
                if isinstance(nested, NamespacedType):
                    children = describe(
                        get_message("/".join((*nested.namespaces, nested.name))),
                        depth + 1,
                    )
                result.append(
                    {
                        "name": name,
                        "type": text,
                        "array": isinstance(slot, AbstractNestedType),
                        "children": children,
                    }
                )
            return result

        return {"topic": topic, "type": message_type, "fields": describe(cls)}

    def configure(self, client: Client, requests: list[dict]):
        self.call(lambda: self._configure(client, requests))

    def _configure(self, client, requests):
        from rclpy.qos import QoSProfile, ReliabilityPolicy, DurabilityPolicy
        from rosidl_runtime_py.utilities import get_message
        from rosidl_runtime_py.convert import message_to_ordereddict
        from rclpy.serialization import serialize_message

        available = dict(self.node.get_topic_names_and_types())
        resolved = [
            (r, get_message(available[r["topic"]][0]))
            for r in requests
            if r["topic"] in available
        ]
        if len(resolved) != len(requests):
            raise ValueError("A requested Topic is no longer available")
        new = []
        try:
            for request, cls in resolved:
                topic, fields = request["topic"], request["fields"]
                publishers = self.node.get_publishers_info_by_topic(topic)
                reliability = request.get("qos", "auto")
                best_effort = reliability == "best_effort" or (
                    reliability == "auto"
                    and any(
                        p.qos_profile.reliability == ReliabilityPolicy.BEST_EFFORT
                        for p in publishers
                    )
                )
                qos = QoSProfile(
                    depth=100,
                    reliability=ReliabilityPolicy.BEST_EFFORT
                    if best_effort
                    else ReliabilityPolicy.RELIABLE,
                    durability=DurabilityPolicy.VOLATILE,
                )
                seq = [0]

                def callback(msg, topic=topic, fields=fields, seq=seq):
                    recv = time.time_ns()
                    source = stamp_ns(msg)
                    now = time.monotonic()
                    seq[0] += 1
                    sample = {
                        "topic": topic,
                        "seq": seq[0],
                        "t": (source or recv) / 1e9,
                        "source_ns": str(source) if source else None,
                        "receive_ns": str(recv),
                        "clock": "source" if source else "receive",
                        "values": {
                            field: value
                            for field in fields
                            if (value := read_path(msg, field)) is not None
                        },
                    }
                    snapshot = None
                    if now - client.last_snapshot.get(topic, 0) >= 1 / 30:
                        data = safe_json(message_to_ordereddict(msg))
                        snapshot = {
                            "topic": topic,
                            "t": sample["t"],
                            "data": data,
                            "source_ns": sample["source_ns"],
                            "receive_ns": str(recv),
                            "clock": sample["clock"],
                        }
                        client.last_snapshot[topic] = now
                    stat = self.stats.setdefault(topic, deque(maxlen=5000))
                    # Size is measured from the actual serialized ROS payload.
                    stat.append((now, len(serialize_message(msg)), recv))
                    client.push(sample, snapshot)

                new.append(self.node.create_subscription(cls, topic, callback, qos))
        except Exception:
            for sub in new:
                self.node.destroy_subscription(sub)
            raise
        self._detach(client)
        self.subs[client] = new
        client.active = requests

    def _detach(self, client):
        for sub in self.subs.pop(client, []):
            self.node.destroy_subscription(sub)
        client.active = []

    def detach(self, client):
        self.call(lambda: self._detach(client))

    def close(self):
        self.running = False
        self.thread.join(timeout=12)
