"""The only browser-to-ROS write contract. Arbitrary ROS operations stay disabled."""
from __future__ import annotations

import math


def control_request(packet: dict) -> dict:
    allowed = {"v", "kind", "operation", "axes", "buttons", "shoulders", "action"}
    if set(packet) - allowed:
        raise ValueError("Unknown web control field")
    operation = packet.get("operation")
    if operation not in ("activate", "release", "input"):
        raise ValueError("Expected activate, release or input")
    if operation != "input":
        if set(packet) - {"v", "kind", "operation"}:
            raise ValueError("Activation and release cannot carry input")
        return {"operation": operation}
    axes, buttons = packet.get("axes"), packet.get("buttons")
    if (
        not isinstance(axes, list) or len(axes) != 4
        or any(type(x) not in (int, float) or not math.isfinite(x) or abs(x) > 1 for x in axes)
        or not isinstance(buttons, list) or len(buttons) != 10
        or any(type(x) is not bool for x in buttons)
        or type(packet.get("shoulders")) is not int
        or not 0 <= packet["shoulders"] <= 3
    ):
        raise ValueError("Expected four normalized axes, ten buttons and shoulders 0..3")
    result = {"operation": operation, "axes": axes, "buttons": buttons, "shoulders": packet["shoulders"]}
    if "action" in packet:
        if not isinstance(packet["action"], str) or not 1 <= len(packet["action"]) <= 32:
            raise ValueError("Invalid web action")
        result["action"] = packet["action"]
    return result
