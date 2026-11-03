#!/usr/bin/env python3
"""Controlled ROS2→WebSocket acceptance; optionally starts official Unitree MuJoCo.
All spawned processes and evidence belong to this invocation. No control publisher.
Run with ui/.venv/bin/python from a sourced ROS environment.
"""

import argparse
import asyncio
import json
import os
import select
import signal
import subprocess
import threading
import time
from pathlib import Path

import websockets

ROOT = Path(__file__).resolve().parents[1]
WORKSPACE = ROOT.parents[1]
parser = argparse.ArgumentParser()
parser.add_argument("--mujoco", action="store_true")
parser.add_argument("--duration", type=float, default=60)
parser.add_argument("--ros-domain", type=int, default=193)
parser.add_argument("--dds-domain", type=int, default=39)
parser.add_argument("--port", type=int, default=8766)
args = parser.parse_args()
ARTIFACTS = ROOT / "artifacts" / ("mujoco" if args.mujoco else "ros-synthetic")
ARTIFACTS.mkdir(parents=True, exist_ok=True)
children, handles = [], []
stop = threading.Event()


def spawn(name, command, env=None, **kwargs):
    output = (ARTIFACTS / f"{name}.log").open("w")
    handles.append(output)
    child = subprocess.Popen(
        command,
        stdout=output,
        stderr=subprocess.STDOUT,
        env=env,
        start_new_session=True,
        **kwargs,
    )
    children.append(child)
    return child


def synthetic():
    import math
    import rclpy
    from sensor_msgs.msg import JointState

    rclpy.init()
    node = rclpy.create_node("telemetry_acceptance_publisher", enable_rosout=False)
    pub = node.create_publisher(JointState, "/telemetry_test/joints", 100)
    index, deadline = 0, time.perf_counter()
    while not stop.is_set():
        msg = JointState()
        msg.header.stamp = node.get_clock().now().to_msg()
        msg.name = [f"joint_{n}" for n in range(12)]
        msg.position = [math.sin(index / 500 * 2 + j) for j in range(12)]
        pub.publish(msg)
        index += 1
        deadline += 0.002
        stop.wait(max(0, deadline - time.perf_counter()))
    node.destroy_node()
    rclpy.shutdown()


async def check():
    url = f"ws://127.0.0.1:{args.port}/ws"
    for _ in range(100):
        try:
            connection = await websockets.connect(url, max_size=8_000_000)
            break
        except OSError:
            await asyncio.sleep(0.1)
    else:
        raise RuntimeError("Gateway failed to start")
    async with connection as ws:
        topic = "/anakin/state" if args.mujoco else "/telemetry_test/joints"
        fields = (
            ([f"joints.position[{i}]" for i in range(11)] + ["state_seq"])
            if args.mujoco
            else [f"position[{i}]" for i in range(12)]
        )

        async def send(**packet):
            await ws.send(json.dumps({"v": 1, **packet}))

        started = time.monotonic()
        found, schema, sent, snapshots, samples = False, False, False, 0, 0
        first_t = last_t = None
        first_wall = last_wall = None
        joints = []
        source_gaps = 0
        first_source_seq = last_source_seq = None
        missing = 0
        dropped = 0
        field_ranges = {f: [float("inf"), float("-inf")] for f in fields}
        while time.monotonic() - started < args.duration + 12:
            packet = json.loads(await asyncio.wait_for(ws.recv(), timeout=12))
            if packet["kind"] == "error":
                raise RuntimeError(packet)
            if packet["kind"] == "topics" and any(
                t["name"] == topic for t in packet["topics"]
            ):
                found = True
                if not sent:
                    await send(kind="schema", topic=topic)
                    await send(
                        kind="subscribe",
                        subscriptions=[
                            {"topic": topic, "fields": fields, "qos": "auto"}
                        ],
                    )
                    sent = True
            if packet["kind"] == "schema":
                schema = bool(packet["fields"])
            if packet["kind"] == "batch":
                dropped = packet["dropped"]
                for sample in packet["samples"]:
                    if sample["topic"] != topic:
                        continue
                    t = sample["t"]
                    if first_t is None:
                        first_t, first_wall = t, time.monotonic()
                    last_t, last_wall = t, time.monotonic()
                    samples += 1
                    missing += len(fields) - len(sample["values"])
                    if args.mujoco:
                        current_seq = sample["values"]["state_seq"]
                        if last_source_seq is not None:
                            source_gaps += max(
                                0, int(current_seq - last_source_seq - 1)
                            )
                        else:
                            first_source_seq = current_seq
                        last_source_seq = current_seq
                    assert sample.get("source_ns") and sample.get("receive_ns"), (
                        "timestamps lost"
                    )
                    for f, value in sample["values"].items():
                        field_ranges[f][0] = min(field_ranges[f][0], value)
                        field_ranges[f][1] = max(field_ranges[f][1], value)
                for snap in packet["snapshots"]:
                    if snap["topic"] == topic:
                        snapshots += 1
                        state = snap["data"]
                        joint_data = state.get("joints", state)
                        joints = joint_data.get("name", [])
                        if args.mujoco:
                            assert state["execution_authorized"] is False
                            assert state["sdk_publish_count"] == 0, (
                                "simulation unexpectedly armed"
                            )
                if first_wall and time.monotonic() - first_wall >= args.duration:
                    break
        assert found and schema and samples > 100 and snapshots > 1, (
            "missing discovery, schema or data"
        )
        assert missing == 0 and dropped == 0
        if args.mujoco:
            model = json.loads((ROOT / "public/models/g1/model.json").read_text())
            assert set(joints) == set(model["joint_names"]), (
                "named joint mapping differs from model"
            )
        result = {
            "input": "official Unitree MuJoCo LowState → existing env ROS2 → gateway WebSocket"
            if args.mujoco
            else "controlled rclpy JointState publisher at 500 Hz → gateway WebSocket",
            "ros_domain": args.ros_domain,
            "dds_domain": args.dds_domain if args.mujoco else None,
            "read_only": True,
            "duration_seconds": last_wall - first_wall,
            "samples": samples,
            "received_hz_wall": (samples - 1) / (last_wall - first_wall),
            "received_hz_source": (samples - 1) / (last_t - first_t),
            "channels": len(fields),
            "snapshots": snapshots,
            "joint_count": len(joints),
            "missing_values": missing,
            "gateway_drops": dropped,
            "field_ranges": field_ranges,
            "upstream_ros_sequence_gaps": source_gaps if args.mujoco else None,
            "observed_ros_publish_hz": (last_source_seq - first_source_seq)
            / (last_wall - first_wall)
            if args.mujoco
            else None,
            "hardware_tested": False,
        }
        (ARTIFACTS / "result.json").write_text(json.dumps(result, indent=2) + "\n")
        print(json.dumps(result, indent=2), flush=True)
        if args.mujoco:
            assert source_gaps == 0, (
                "ROS publisher sequence gaps in the received stream"
            )
        else:
            assert result["received_hz_wall"] >= 450, (
                "less than 90% of the 500Hz target"
            )


try:
    os.environ.update(
        ROS_DOMAIN_ID=str(args.ros_domain),
        ROS_LOCALHOST_ONLY="1",
        RMW_IMPLEMENTATION="rmw_fastrtps_cpp",
    )
    env = os.environ.copy()
    if args.mujoco:
        sim_root = WORKSPACE / "unitree_mujoco"
        xvfb = Path("/usr/bin/Xvfb")
        if not xvfb.exists():
            xvfb = WORKSPACE / "Anakin/build/unitree_mujoco-deps/xvfb/usr/bin/Xvfb"
        read_fd, write_fd = os.pipe()
        x = spawn(
            "xvfb",
            [
                str(xvfb),
                "-displayfd",
                str(write_fd),
                "-screen",
                "0",
                "1280x800x24",
                "-nolisten",
                "tcp",
                "-noreset",
            ],
            pass_fds=(write_fd,),
        )
        os.close(write_fd)
        if not select.select([read_fd], [], [], 8)[0]:
            raise RuntimeError("Xvfb startup timed out")
        display = os.read(read_fd, 32).decode().strip()
        os.close(read_fd)
        env["DISPLAY"] = ":" + display
        spawn(
            "simulator",
            [
                str(sim_root / "simulate/build/unitree_mujoco"),
                "-r",
                "g1",
                "-i",
                str(args.dds_domain),
                "-n",
                "lo",
                "-t",
                "1",
                "-s",
                str(sim_root / "unitree_robots/g1/scene_29dof.xml"),
            ],
            env=env,
        )
        env["ANAKIN_PREFIX"] = str(ROOT.parent / "install/anakin_env")
        spawn(
            "env",
            [
                str(ROOT.parent / "en_run.sh"),
                "--loopback-sim",
                "-p",
                f"sdk_domain:={args.dds_domain}",
                "-p",
                f"duration:={args.duration + 30}",
                "-p",
                "min_active_seconds:=0.0",
            ],
            env=env,
        )
    else:
        threading.Thread(target=synthetic, daemon=True).start()
    gateway_env = os.environ.copy()
    gateway_env["TELEMETRY_PORT"] = str(args.port)
    gateway_env["TELEMETRY_ROS_SETUP"] = str(
        WORKSPACE / "anakin_cpp_ws/install/anakin_sdk/share/anakin_sdk/local_setup.bash"
    )
    spawn("gateway", [str(ROOT / "scripts/gateway.sh")], env=gateway_env, cwd=ROOT)
    asyncio.run(check())
finally:
    stop.set()
    for child in reversed(children):
        if child.poll() is None:
            os.killpg(child.pid, signal.SIGTERM)
            try:
                child.wait(timeout=6)
            except subprocess.TimeoutExpired:
                os.killpg(child.pid, signal.SIGKILL)
                child.wait()
    for handle in handles:
        handle.close()
