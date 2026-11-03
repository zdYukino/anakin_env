from __future__ import annotations

import asyncio
import logging
import os
import time
from contextlib import asynccontextmanager, suppress
from pathlib import Path

from fastapi import FastAPI, WebSocket, WebSocketDisconnect
from fastapi.staticfiles import StaticFiles

from .protocol import Client, VERSION, event, subscriptions

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
            "read_only": True,
        }

    @app.websocket("/ws")
    async def websocket(ws: WebSocket):
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
                read_only=True,
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
                    if not isinstance(packet, dict) or packet.get("v") != VERSION:
                        raise ValueError("Expected protocol version 1")
                    kind = packet.get("kind")
                    if kind == "subscribe":
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
                            "Unsupported operation. This gateway is read-only."
                        )
                except (
                    ValueError,
                    KeyError,
                    TypeError,
                    ImportError,
                    RuntimeError,
                ) as exc:
                    await send(
                        event("error", code="REQUEST_REJECTED", message=str(exc))
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
                                "Stream stale" if stale else "Read-only ROS2 gateway"
                            ),
                        )
                    )
                    last_graph = time.monotonic()
                packet = client.drain()
                if packet["samples"] or packet["snapshots"]:
                    await send(packet)
                await asyncio.sleep(1 / 30)

        receiver, sender = asyncio.create_task(receive()), asyncio.create_task(stream())
        try:
            done, pending = await asyncio.wait(
                [receiver, sender], return_when=asyncio.FIRST_COMPLETED
            )
            for task in done:
                task.result()
        except (WebSocketDisconnect, TimeoutError, asyncio.TimeoutError):
            pass
        except Exception as exc:
            log.warning("Client disconnected: %s", exc)
        finally:
            for task in (receiver, sender):
                task.cancel()
                with suppress(asyncio.CancelledError, Exception):
                    await task
            with suppress(Exception):
                await asyncio.to_thread(adapter.detach, client)

    dist = Path(__file__).resolve().parents[1] / "dist"
    if dist.exists():
        app.mount("/", StaticFiles(directory=dist, html=True), name="web")
    return app


app = create_app()
