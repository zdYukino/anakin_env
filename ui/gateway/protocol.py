"""Transport-independent v1 primitives. No ROS imports in this module."""

from __future__ import annotations

import math
import re
import threading
import time
from collections import deque
from typing import Any, Protocol

VERSION = 1
MAX_TOPICS = 20
MAX_FIELDS = 12
MAX_QUEUE = 5000
PATH = re.compile(r"^[A-Za-z_][\w]*(?:\[\d+\]|\.[A-Za-z_][\w]*)*$")


class Adapter(Protocol):
    def graph(self) -> list[dict]: ...
    def input_config(self) -> dict: ...
    def control(self, client: "Client", request: dict) -> dict: ...
    def control_status(self, client: "Client") -> dict: ...
    def release_control(self, client: "Client") -> None: ...
    def schema(self, topic: str) -> dict: ...
    def configure(self, client: "Client", subscriptions: list[dict]) -> None: ...
    def detach(self, client: "Client") -> None: ...
    def close(self) -> None: ...


def event(kind: str, **payload: Any) -> dict:
    return {"v": VERSION, "kind": kind, **payload}


def subscriptions(value: Any) -> list[dict]:
    if not isinstance(value, list) or len(value) > MAX_TOPICS:
        raise ValueError(
            f"subscriptions must be an array of at most {MAX_TOPICS} topics"
        )
    count, seen = 0, set()
    for item in value:
        if not isinstance(item, dict):
            raise ValueError("Each subscription must be an object")
        topic, fields = item.get("topic"), item.get("fields")
        if (
            not isinstance(topic, str)
            or not topic.startswith("/")
            or len(topic) > 256
            or topic in seen
        ):
            raise ValueError("Topic must be a unique absolute ROS topic name")
        seen.add(topic)
        if not isinstance(fields, list) or any(
            not isinstance(f, str) or len(f) > 256 or not PATH.fullmatch(f)
            for f in fields
        ):
            raise ValueError("Invalid field path; use field.child[0]")
        if len(fields) != len(set(fields)):
            raise ValueError("Duplicate field path")
        if item.get("qos", "auto") not in ("auto", "best_effort", "reliable"):
            raise ValueError("Unknown subscription reliability")
        count += len(fields)
    if count > MAX_FIELDS:
        raise ValueError(f"At most {MAX_FIELDS} numeric fields can be streamed")
    return value


def read_path(message: Any, path: str) -> float | None:
    try:
        value = message
        for name, index in re.findall(r"([A-Za-z_]\w*)|\[(\d+)\]", path):
            value = getattr(value, name) if name else value[int(index)]
        if isinstance(value, (str, bytes, bool)):
            return None
        number = float(value)
        return number if math.isfinite(number) else None
    except (AttributeError, IndexError, KeyError, TypeError, ValueError, OverflowError):
        return None


def stamp_ns(message: Any) -> int | None:
    header = getattr(message, "header", None)
    stamp = getattr(header, "stamp", None)
    if stamp is None:
        return None
    result = int(stamp.sec) * 1_000_000_000 + int(stamp.nanosec)
    return result if result > 0 else None


def safe_json(value: Any) -> Any:
    if isinstance(value, dict):
        return {str(k): safe_json(v) for k, v in value.items()}
    if isinstance(value, (list, tuple)):
        return [safe_json(v) for v in value]
    if isinstance(value, float) and not math.isfinite(value):
        return None
    return value


class Client:
    """Thread-safe bounded queue. ROS never blocks on a slow websocket."""

    def __init__(self, capacity: int = MAX_QUEUE):
        self.lock = threading.Lock()
        self.queue: deque[dict] = deque(maxlen=capacity)
        self.snapshots: dict[str, dict] = {}
        self.last_snapshot: dict[str, float] = {}
        self.dropped = 0
        self.received = 0
        self.last_received = 0.0
        self.active: list[dict] = []

    def push(self, sample: dict, snapshot: dict | None = None):
        with self.lock:
            if len(self.queue) == self.queue.maxlen:
                self.dropped += 1
            self.queue.append(sample)
            if snapshot is not None:
                self.snapshots[sample["topic"]] = snapshot
            self.received += 1
            self.last_received = time.monotonic()

    def drain(self) -> dict:
        with self.lock:
            samples = list(self.queue)
            snapshots = list(self.snapshots.values())
            self.queue.clear()
            self.snapshots.clear()
            return event(
                "batch",
                samples=samples,
                snapshots=snapshots,
                dropped=self.dropped,
                received=self.received,
            )
