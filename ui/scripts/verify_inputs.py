#!/usr/bin/env python3
"""Verify native input/config and optional leased web control on isolated domains.

No simulator, MC or robot feedback; every test-mode LowCmd output is disabled.
--events exercises only this process's ENV through the normal WebSocket contract.
"""
import argparse
import asyncio
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import tempfile
import time

import websockets
import yaml

ROOT = Path(__file__).resolve().parents[1]
ENV_ROOT = ROOT.parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--ros-domain", type=int, default=196)
parser.add_argument("--dds-domain", type=int, default=43)
parser.add_argument("--port", type=int, default=8768)
parser.add_argument("--preview-seconds", type=float, default=0)
parser.add_argument("--events", action="store_true", help="Exercise explicitly leased web commands with all LowCmd outputs disabled")
parser.add_argument("--gamepad", action="store_true", help="Also monitor a local physical gamepad")
args = parser.parse_args()
if args.ros_domain == 87 or args.dds_domain in (0, 31):
    parser.error("Choose isolated domains, outside the normal robot/simulation session")


def interrupted(signum, _frame):
    raise KeyboardInterrupt(f"Verification interrupted by signal {signum}")


signal.signal(signal.SIGTERM, interrupted)
NEUTRAL = {"v": 1, "kind": "control", "operation": "input", "axes": [0, 0, 0, 0], "buttons": [False] * 10, "shoulders": 0}


async def verify(input_path):
    for _ in range(80):
        try:
            ws = await websockets.connect(f"ws://127.0.0.1:{args.port}/ws")
            break
        except OSError:
            await asyncio.sleep(0.1)
    else:
        raise RuntimeError("Gateway did not start")
    requested = False

    async def heartbeat():
        while True:
            if requested:
                await ws.send(json.dumps(NEUTRAL))
            await asyncio.sleep(.05)

    async with ws:
        task = asyncio.create_task(heartbeat())
        try:
            started = time.monotonic()
            config = None
            reports = []
            subscribed = edited = confirmed = False
            stages = [("disarmed", "disarmed", "success"), ("asap_loco", "asap_loco", "pending"), ("emergency_stop", "damping", "success")]
            stage = 0
            sent = -1
            after_sequence = 0
            observed = []
            rejected = False
            rejection_sent = False
            control = {}
            released = False
            while time.monotonic() - started < 20:
                if children and children[0].poll() is not None:
                    raise RuntimeError("Isolated ENV exited before verification")
                packet = json.loads(await asyncio.wait_for(ws.recv(), 5))
                if packet["kind"] == "error":
                    if packet.get("code") == "CONTROL_REJECTED" and rejection_sent:
                        rejected = True
                    else:
                        raise RuntimeError(packet)
                elif packet["kind"] == "control_state":
                    control = packet
                    if control.get("available"):
                        assert control["sdk_publish_count"] == 0, "Verification must never publish SDK commands"
                elif packet["kind"] == "topics" and not subscribed and any(topic["name"] == "/anakin/input_state" for topic in packet["topics"]):
                    await ws.send(json.dumps({"v": 1, "kind": "subscribe", "subscriptions": [{"topic": "/anakin/input_state", "fields": [], "qos": "auto"}]}))
                    subscribed = True
                elif packet["kind"] == "input_config" and packet.get("available"):
                    config = packet
                    assert config["path"] == str(input_path)
                    assert config["config"]["schema_version"] == 2 and config["config"]["primary"] == "auto"
                    assert "keyboard_bindings" not in config["config"]
                    binding = config["config"]["controller_bindings"][0]
                    assert binding["button"] == "Start" and binding["trigger"] == "released"
                    if edited:
                        confirmed = True
                    else:
                        changed = yaml.safe_load(input_path.read_text())
                        changed["controller_bindings"][0]["button"] = "Select"
                        input_path.write_text(yaml.safe_dump(changed))
                        edited = True
                elif packet["kind"] == "batch":
                    for snapshot in packet["snapshots"]:
                        assert snapshot["topic"] == "/anakin/input_state"
                        states = {status["hardware_id"]: {item["key"]: item["value"] for item in status["values"]} for status in snapshot["data"]["status"] if status["name"].startswith("anakin_env/input/")}
                        assert set(states) == {"gamepad", "remote", "web"}
                        assert states["remote"]["online"] == "false"
                        reports.append(snapshot)
                        if args.events and sent == stage and stage < len(stages):
                            for status in snapshot["data"]["status"]:
                                if status["name"] != "anakin_env/switch_event":
                                    continue
                                event = {item["key"]: item["value"] for item in status["values"]}
                                if int(event["sequence"]) > after_sequence and event["source"] == "web" and (event["target"], event["result"]) == stages[stage][1:]:
                                    observed.append(event)
                                    after_sequence = int(event["sequence"])
                                    stage += 1
                                    break
                ready = confirmed and len(reports) >= 30 and control.get("available")
                if args.events and ready:
                    assert control["actions"]["hold"] is False, "YAML must gray out the removed Hold route"
                    if not requested and not released:
                        await ws.send(json.dumps({"v": 1, "kind": "control", "operation": "activate"}))
                        requested = True
                    if control.get("owned") and stage < len(stages) and sent != stage:
                        if stage == 2 and not rejected:
                            if not rejection_sent:
                                await ws.send(json.dumps({**NEUTRAL, "action": "hold"}))
                                rejection_sent = True
                        else:
                            await ws.send(json.dumps({**NEUTRAL, "action": stages[stage][0]}))
                            sent = stage
                    if stage == len(stages) and not released:
                        requested = False
                        await ws.send(json.dumps({"v": 1, "kind": "control", "operation": "release"}))
                        released = True
                        continue
                if ready and (not args.events or (released and not control.get("owned") and not control.get("leased"))):
                    span = reports[-1]["t"] - reports[0]["t"]
                    result = {"input": "native ENV → ROS2 → Gateway → WebSocket", "ros_domain": args.ros_domain, "dds_domain": args.dds_domain,
                              "snapshots": len(reports), "snapshot_hz": round((len(reports) - 1) / span, 2),
                              "effective_primary": config["config"]["primary"], "disk_edit_did_not_replace_running_config": True,
                              "hardware_tested": False, "sdk_publish_count": control.get("sdk_publish_count"),
                              "verified_events": observed, "unavailable_action_rejected": rejected,
                              "preview_url": f"http://127.0.0.1:{args.port}/"}
                    assert result["snapshot_hz"] > 20
                    print(json.dumps(result, indent=2), flush=True)
                    return
            raise RuntimeError("Missing input metadata, web lease or state events")
        finally:
            task.cancel()
            try:
                await task
            except asyncio.CancelledError:
                pass


children = []
with socket.socket() as reservation:
    reservation.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    reservation.bind(("127.0.0.1", args.port))
with tempfile.TemporaryDirectory(prefix="anakin-input-verify-") as directory:
    folder = Path(directory)
    environment = os.environ.copy()
    environment.update(ROS_DOMAIN_ID=str(args.ros_domain), ROS_LOCALHOST_ONLY="1", RMW_IMPLEMENTATION="rmw_fastrtps_cpp")
    mode = yaml.safe_load((ENV_ROOT / "config/sim.yaml").read_text())
    mode["transport"].update(interface="lo", sdk_domain=args.dds_domain, ros_domain=args.ros_domain)
    for state in mode["states"].values():
        state["outputs"]["lowcmd"] = False
        state["on_enter"] = ["revoke_actuation", "release_if_owned"] + [action for action in state["on_enter"] if action not in ("revoke_actuation", "release_if_owned")]
    if args.events:
        for route in mode["manual_transitions"]:
            route["to"] = [state for state in route["to"] if state != "hold"]
    mode_path = folder / "sim.yaml"
    mode_path.write_text(yaml.safe_dump(mode))
    inputs = yaml.safe_load((ENV_ROOT / "config/inputs.yaml").read_text())
    inputs["enabled_sources"] = ["web", "remote"] + (["gamepad"] if args.gamepad else [])
    inputs["primary"] = "auto"
    inputs["controller_bindings"][0].update(button="Start", shoulders=["R1"], trigger="released")
    input_path = folder / "selected-inputs.yaml"
    input_path.write_text(yaml.safe_dump(inputs))
    logs = []
    try:
        for name, command in [
            ("env", [str(ENV_ROOT / "install/anakin_env/lib/anakin_env/anakin_env_native_node"), "--sim", "--config", str(mode_path), "--input-config", str(input_path)]),
            ("gateway", [str(ROOT / ".venv/bin/python"), "-m", "uvicorn", "gateway.server:app", "--host", "127.0.0.1", "--port", str(args.port)]),
        ]:
            handle = (folder / f"{name}.log").open("w")
            logs.append(handle)
            children.append(subprocess.Popen(command, cwd=ROOT, env=environment, stdin=subprocess.DEVNULL, stdout=handle, stderr=subprocess.STDOUT, start_new_session=True))
        asyncio.run(verify(input_path))
        input_path.write_text(yaml.safe_dump(inputs))
        deadline = time.monotonic() + args.preview_seconds
        while time.monotonic() < deadline:
            time.sleep(.25)
    except Exception:
        for name in ("env", "gateway"):
            print((folder / f"{name}.log").read_text())
        raise
    finally:
        for child in reversed(children):
            if child.poll() is None:
                os.killpg(child.pid, signal.SIGTERM)
                try:
                    child.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    os.killpg(child.pid, signal.SIGKILL)
                    child.wait()
        for handle in logs:
            handle.close()
