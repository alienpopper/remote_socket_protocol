#!/usr/bin/env python3
"""Python HTTP server over RSP using the Python client."""

import asyncio
import json
import os
import signal
import sys
from pathlib import Path




def _find_repo_root() -> Path:
    candidate = Path(__file__).resolve()
    for parent in candidate.parents:
        if (parent / "client" / "python" / "rsp_client.py").exists():
            return parent
    raise RuntimeError("failed to locate repository root for python client imports")


REPO_ROOT = _find_repo_root()
CLIENT_PYTHON_DIR = REPO_ROOT / "client" / "python"
if str(CLIENT_PYTHON_DIR) not in sys.path:
    sys.path.insert(0, str(CLIENT_PYTHON_DIR))

from rsp_client import RSPClient  # noqa: E402
from rsp_net import create_server  # noqa: E402


def _build_http_response(path: str) -> bytes:
    if path == "/healthz":
        body = json.dumps({"ok": True, "transport": "rsp"}).encode("utf-8")
        headers = [
            b"HTTP/1.1 200 OK",
            b"Content-Type: application/json; charset=utf-8",
            f"Content-Length: {len(body)}".encode("ascii"),
            b"Connection: close",
            b"",
            b"",
        ]
        return b"\r\n".join(headers) + body

    body = b"""<!doctype html>
<html>
  <head>
    <meta charset=\"utf-8\" />
    <title>RSP Python HTTP Server</title>
  </head>
  <body>
    <h1>Remote Socket Protocol</h1>
    <p>Python HTTP server is serving this page through RS/RM transport.</p>
  </body>
</html>"""
    headers = [
        b"HTTP/1.1 200 OK",
        b"Content-Type: text/html; charset=utf-8",
        f"Content-Length: {len(body)}".encode("ascii"),
        b"Connection: close",
        b"",
        b"",
    ]
    return b"\r\n".join(headers) + body


async def _read_http_request(socket, timeout_s: float = 5.0) -> bytes:
    data = bytearray()
    while b"\r\n\r\n" not in data and len(data) < (128 * 1024):
        chunk = await asyncio.wait_for(socket.read(), timeout=timeout_s)
        if not chunk:
            break
        data.extend(chunk)
    return bytes(data)


def _extract_request_path(raw_request: bytes) -> str:
    try:
        request_line = raw_request.split(b"\r\n", 1)[0].decode("ascii", errors="ignore")
        parts = request_line.split(" ")
        if len(parts) >= 2 and parts[1].startswith("/"):
            return parts[1]
    except Exception:
        pass
    return "/"


async def _handle_connection(socket) -> None:
    try:
        print("[python-rsp] accepted connection", file=sys.stderr, flush=True)
        request_bytes = await _read_http_request(socket)
        print(f"[python-rsp] request bytes={len(request_bytes)}", file=sys.stderr, flush=True)
        path = _extract_request_path(request_bytes)
        response = _build_http_response(path)
        await socket.write(response)
        print(f"[python-rsp] response bytes={len(response)}", file=sys.stderr, flush=True)
    finally:
        await socket.close()
        print("[python-rsp] connection closed", file=sys.stderr, flush=True)


async def start_over_rsp() -> None:
    transport_spec = os.environ.get("RSP_TRANSPORT")
    resource_service_node_id = os.environ.get("RSP_RESOURCE_SERVICE_NODE_ID")
    endorsement_node_id = os.environ.get("RSP_ENDORSEMENT_NODE_ID")
    host_port = os.environ.get("RSP_HOST_PORT", "127.0.0.1:8080")

    if not transport_spec or not resource_service_node_id:
        raise RuntimeError("RSP_TRANSPORT and RSP_RESOURCE_SERVICE_NODE_ID are required")

    stop_event = asyncio.Event()

    def request_stop() -> None:
        stop_event.set()

    loop = asyncio.get_running_loop()
    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            loop.add_signal_handler(sig, request_stop)
        except NotImplementedError:
            signal.signal(sig, lambda *_: request_stop())

    async with RSPClient() as client:
        await client.connect(transport_spec)

        if endorsement_node_id:
            reachable = await client.ping(endorsement_node_id, timeout=3.0)
            if not reachable:
                raise RuntimeError("endorsement service is unreachable")

        server = await create_server(client, resource_service_node_id, host_port)

        connection_tasks = set()

        async def accept_loop() -> None:
            while not stop_event.is_set():
                try:
                    socket = await server.accept()
                except Exception:
                    if stop_event.is_set():
                        return
                    await asyncio.sleep(0.05)
                    continue

                task = asyncio.create_task(_handle_connection(socket))
                connection_tasks.add(task)
                task.add_done_callback(connection_tasks.discard)

        accept_task = asyncio.create_task(accept_loop())

        print(f"Python HTTP server over RSP listening on {host_port}", flush=True)
        await stop_event.wait()

        accept_task.cancel()
        await asyncio.gather(accept_task, return_exceptions=True)
        await server.close()
        if connection_tasks:
            await asyncio.gather(*connection_tasks, return_exceptions=True)


def start_over_tcp() -> None:
    from http.server import BaseHTTPRequestHandler, HTTPServer

    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            body = b"Python HTTP server fallback mode"
            self.send_response(200)
            self.send_header("Content-Type", "text/plain; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    port = int(os.environ.get("PORT", "3000"))
    server = HTTPServer(("127.0.0.1", port), Handler)
    print(f"Python HTTP server listening on 127.0.0.1:{port}", flush=True)
    server.serve_forever()


async def main() -> None:
    if os.environ.get("RSP_TRANSPORT"):
        await start_over_rsp()
    else:
        start_over_tcp()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
