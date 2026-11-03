"""rclpy access lives exclusively in one executor thread."""

from __future__ import annotations

import concurrent.futures
import queue
import threading
import time
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
