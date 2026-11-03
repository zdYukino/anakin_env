from __future__ import annotations

import asyncio
import logging
import os
import time
from contextlib import asynccontextmanager, suppress
from pathlib import Path
from urllib.parse import urlsplit

from fastapi import FastAPI, WebSocket, WebSocketDisconnect
from fastapi.staticfiles import StaticFiles
from anyio import CancelScope

from .protocol import Client, VERSION, event, subscriptions
from .control import control_request

log = logging.getLogger("telemetry")


@asynccontextmanager
async def lifespan(app):
    from .ros_adapter import Ros2Adapter

    app.state.adapter = None
    app.state.error = None
    try:
        app.state.adapter = await asyncio.to_thread(Ros2Adapter)
    except Exception as exc:
        app.state.error = str(exc)
        log.error("Gateway adapter initialization: %s", exc)
    yield
    if app.state.adapter:
        await asyncio.to_thread(app.state.adapter.close)


def create_app(adapter=None):
    app = FastAPI(
        title="Aerospace Telemetry Gateway",
        lifespan=lifespan if adapter is None else None,
    )
    if adapter is not None:
        app.state.adapter, app.state.error = adapter, None

    @app.get("/api/health")
    async def health():
        return {
            "v": VERSION,
            "adapter": "ros2",
            "ready": app.state.adapter is not None,
            "error": app.state.error,
            "domain_id": os.getenv("ROS_DOMAIN_ID", "0"),
            "read_only": False,
            "control_protocol": 1,
        }

    @app.websocket("/ws")
    async def websocket(ws: WebSocket):
        origin = ws.headers.get("origin")
        if origin and (urlsplit(origin).scheme not in ("http", "https") or urlsplit(origin).netloc != ws.headers.get("host")):
            await ws.close(code=1008)
            return
        await ws.accept()
        adapter = app.state.adapter
        if adapter is None:
            await ws.send_json(
                event(
                    "error",
                    code="ROS_UNAVAILABLE",
                    message=app.state.error or "ROS2 adapter not initialized",
                )
            )
            await ws.close(code=1011)
            return
        client = Client()
        await ws.send_json(
            event(
                "hello",
                adapter="ros2",
                read_only=False,
                control_protocol=1,
                domain_id=os.getenv("ROS_DOMAIN_ID", "0"),
            )
        )
        send_lock = asyncio.Lock()

        async def send(packet):
            async with send_lock:
                await asyncio.wait_for(ws.send_json(packet), timeout=3)

        async def receive():
            while True:
                packet = await ws.receive_json()
                try:
                    if not isinstance(packet, dict) or type(packet.get("v")) is not int or packet.get("v") != VERSION:
                        raise ValueError("Expected protocol version 1")
                    kind = packet.get("kind")
                    if kind == "control":
                        if not hasattr(adapter, "control"):
                            raise ValueError("Web control is unavailable")
                        state = await asyncio.to_thread(adapter.control, client, control_request(packet))
                        await send(event("control_state", **state))
                    elif kind == "subscribe":
                        requests = subscriptions(packet.get("subscriptions"))
                        await asyncio.to_thread(adapter.configure, client, requests)
                        await send(event("subscribed", subscriptions=requests))
                    elif kind == "schema":
                        schema = await asyncio.to_thread(
                            adapter.schema, packet.get("topic", "")
                        )
                        await send(event("schema", **schema))
                    elif kind in ("refresh", "ping"):
                        if kind == "refresh":
                            await send(
                                event(
                                    "topics",
                                    topics=await asyncio.to_thread(adapter.graph),
                                )
                            )
                        else:
                            await send(event("pong", receive_ns=str(time.time_ns())))
                    elif kind == "unsubscribe":
                        await asyncio.to_thread(adapter.detach, client)
                    else:
                        raise ValueError(
                            "General ROS operations remain read-only; use explicit leased web control."
                        )
                except (
                    ValueError,
                    KeyError,
                    TypeError,
                    ImportError,
                    RuntimeError,
                ) as exc:
                    await send(
                        event("error", code="CONTROL_REJECTED" if isinstance(packet, dict) and packet.get("kind") == "control" else "REQUEST_REJECTED", message=str(exc))
                    )

        async def stream():
            last_graph = 0
            while True:
                if time.monotonic() - last_graph >= 1:
                    await send(
                        event("topics", topics=await asyncio.to_thread(adapter.graph))
                    )
                    stale = (
                        client.active
                        and client.last_received
                        and time.monotonic() - client.last_received > 2
                    )
                    awaiting = client.active and not client.last_received
                    state = (
                        "DEGRADED"
                        if stale or awaiting or client.dropped
                        else "CONNECTED"
                    )
                    await send(
                        event(
                            "status",
                            state=state,
                            dropped=client.dropped,
                            received=client.received,
                            message="Waiting for subscribed data"
                            if awaiting
                            else (
                                "Stream stale" if stale else "ROS2 telemetry gateway"
                            ),
                        )
                    )
                    last_graph = time.monotonic()
                packet = client.drain()
                if packet["samples"] or packet["snapshots"]:
                    await send(packet)
                await asyncio.sleep(1 / 30)

        async def configuration():
            while True:
                await send(event("input_config", **await asyncio.to_thread(adapter.input_config)))
                await asyncio.sleep(1)

        async def control_state():
            while True:
                if hasattr(adapter, "control_status"):
                    await send(event("control_state", **await asyncio.to_thread(adapter.control_status, client)))
                await asyncio.sleep(0.1)

        receiver, sender, config_sender, control_sender = (
            asyncio.create_task(receive()),
            asyncio.create_task(stream()),
            asyncio.create_task(configuration()),
            asyncio.create_task(control_state()),
        )
        try:
            done, pending = await asyncio.wait(
                [receiver, sender, config_sender, control_sender], return_when=asyncio.FIRST_COMPLETED
            )
            for task in done:
                task.result()
        except (WebSocketDisconnect, TimeoutError, asyncio.TimeoutError):
            pass
        except Exception as exc:
            log.warning("Client disconnected: %s", exc)
        finally:
            # ASGI can cancel the handler while its socket is closing. Releasing
            # the lease must finish even when the surrounding request is cancelled.
            with CancelScope(shield=True):
                for task in (receiver, sender, config_sender, control_sender):
                    task.cancel()
                with suppress(Exception):
                    if hasattr(adapter, "release_control"):
                        await asyncio.to_thread(adapter.release_control, client)
                with suppress(Exception):
                    await asyncio.to_thread(adapter.detach, client)
                for task in (receiver, sender, config_sender, control_sender):
                    with suppress(asyncio.CancelledError, Exception):
                        await task

    dist = Path(__file__).resolve().parents[1] / "dist"
    if dist.exists():
        app.mount("/", StaticFiles(directory=dist, html=True), name="web")
    return app


app = create_app()
