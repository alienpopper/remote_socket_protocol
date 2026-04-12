#!/usr/bin/env python3
"""Python HTTP server over RSP integration test using the Python client."""

import asyncio
import json
import os
import signal
import sys
from pathlib import Path

HARNESS_TIMEOUT_S = 60.0
ENDORSEMENT_SUCCESS = 0
ETYPE_ACCESS = "f1e2d3c4-5b6a-7980-1a2b-3c4d5e6f7a8b"
EVALUE_ACCESS_NETWORK = "f1e2d3c4-5b6a-7980-1a2b-3c4d5e6f7a8b"
ETYPE_ROLE = "0963c0ab-215f-42c1-b042-747bf21e330e"
EVALUE_ROLE_CLIENT = "edab2025-4ae1-44f2-a683-1a390586e10c"
EVALUE_ROLE_RESOURCE_SERVICE = "a7f8c9d6-3b2e-4f1a-8c9d-5e6f7a8b9c0d"


def _repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


CLIENT_PYTHON_DIR = _repo_root() / "client" / "python"
if str(CLIENT_PYTHON_DIR) not in sys.path:
    sys.path.insert(0, str(CLIENT_PYTHON_DIR))

from rsp_client import RSPClient  # noqa: E402
from rsp_net import create_connection  # noqa: E402


async def with_timeout(coro, timeout_s: float, label: str):
    try:
        return await asyncio.wait_for(coro, timeout=timeout_s)
    except asyncio.TimeoutError as exc:
        raise RuntimeError(f"{label} timed out after {timeout_s}s") from exc


async def _drain_stream(stream: asyncio.StreamReader, sink: list[str], prefix: str) -> None:
    while True:
        line = await stream.readline()
        if not line:
            return
        text = line.decode("utf-8", errors="replace").rstrip()
        sink.append(text)
        print(f"[{prefix}] {text}", file=sys.stderr)


async def wait_for_fixture_ready(proc: asyncio.subprocess.Process, stderr_lines: list[str]) -> dict:
    stdout_lines = []
    while proc.returncode is None:
        line = await proc.stdout.readline()
        if not line:
            break
        text = line.decode("utf-8", errors="replace").strip()
        stdout_lines.append(text)
        try:
            return json.loads(text)
        except json.JSONDecodeError:
            continue

    await proc.wait()
    raise RuntimeError(
        "fixture exited before readiness JSON"
        + (f"\nstdout:\n{os.linesep.join(stdout_lines)}" if stdout_lines else "")
        + (f"\nstderr:\n{os.linesep.join(stderr_lines)}" if stderr_lines else "")
    )


async def wait_for_server_ready(proc: asyncio.subprocess.Process) -> None:
    while True:
        line = await proc.stdout.readline()
        if not line:
            stderr_output = b""
            if proc.stderr is not None:
                try:
                    stderr_output = await asyncio.wait_for(proc.stderr.read(), timeout=0.5)
                except Exception:
                    stderr_output = b""
            raise RuntimeError(
                "python server exited before ready"
                + (f"\nstderr:\n{stderr_output.decode('utf-8', errors='replace')}" if stderr_output else "")
            )
        text = line.decode("utf-8", errors="replace").strip()
        if "Python HTTP server over RSP listening on" in text:
            return


async def read_until_expected(socket, expected_tokens: list[str], timeout_s: float) -> str:
    deadline = asyncio.get_running_loop().time() + timeout_s
    chunks = bytearray()
    while True:
        text = chunks.decode("utf-8", errors="replace")
        if all(token in text for token in expected_tokens):
            return text

        remaining = deadline - asyncio.get_running_loop().time()
        if remaining <= 0:
            raise RuntimeError("timed out waiting for expected HTTP response tokens")

        chunk = await asyncio.wait_for(socket.read(), timeout=min(1.0, remaining))
        if not chunk:
            return chunks.decode("utf-8", errors="replace")
        chunks.extend(chunk)


async def request_endorsement_or_throw(client: RSPClient, endorsement_node_id: str, endorsement_type: str, endorsement_value: str, label: str) -> None:
    value_bytes = endorsement_value.encode("utf-8")
    reply = None
    for _ in range(3):
        reply = await client.begin_endorsement_request(endorsement_node_id, endorsement_type, value_bytes)
        if reply and reply.get("status") == ENDORSEMENT_SUCCESS:
            return
    status = reply.get("status") if isinstance(reply, dict) else "null"
    raise RuntimeError(f"{label} endorsement failed (status={status})")


async def terminate_process(proc: asyncio.subprocess.Process) -> None:
    if proc is None or proc.returncode is not None:
        return
    proc.terminate()
    try:
        await asyncio.wait_for(proc.wait(), timeout=2.0)
    except asyncio.TimeoutError:
        proc.kill()
        await proc.wait()


async def main() -> None:
    if len(sys.argv) < 2:
        raise RuntimeError("Usage: python3 test/python_http_server_integration.py <fixture-path>")

    fixture_path = sys.argv[1]
    fixture = await asyncio.create_subprocess_exec(
        fixture_path,
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.PIPE,
    )

    fixture_stderr_lines: list[str] = []
    fixture_stderr_task = asyncio.create_task(_drain_stream(fixture.stderr, fixture_stderr_lines, "fixture:stderr"))

    server_proc = None
    server_stderr_task = None
    server_stderr_lines: list[str] = []
    requester_client = None

    try:
        print("[python_http_server_integration] waiting for fixture", file=sys.stderr)
        info = await with_timeout(wait_for_fixture_ready(fixture, fixture_stderr_lines), 30.0, "fixture readiness")
        print("[python_http_server_integration] fixture ready", file=sys.stderr)

        host_port = "127.0.0.1:51880"
        app_path = _repo_root() / "integration" / "python_http_server" / "working" / "app.py"

        env = os.environ.copy()
        env.update({
            "RSP_TRANSPORT": info["transport_spec"],
            "RSP_RESOURCE_SERVICE_NODE_ID": info["resource_service_node_id"],
            "RSP_ENDORSEMENT_NODE_ID": info["endorsement_service_node_id"],
            "RSP_HOST_PORT": host_port,
        })

        server_proc = await asyncio.create_subprocess_exec(
            sys.executable,
            str(app_path),
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE,
            env=env,
        )

        server_stderr_task = asyncio.create_task(_drain_stream(server_proc.stderr, server_stderr_lines, "python-server:stderr"))

        print("[python_http_server_integration] waiting for python server", file=sys.stderr)
        await with_timeout(wait_for_server_ready(server_proc), 25.0, "python server readiness")
        print("[python_http_server_integration] python server ready", file=sys.stderr)

        requester_client = RSPClient()
        print("[python_http_server_integration] connecting requester", file=sys.stderr)
        await with_timeout(requester_client.connect(info["transport_spec"]), 5.0, "requester connect")
        print("[python_http_server_integration] requester connected", file=sys.stderr)

        reachable = await with_timeout(
            requester_client.ping(info["endorsement_service_node_id"], timeout=3.0),
            4.0,
            "endorsement service ping",
        )
        if not reachable:
            raise RuntimeError("endorsement service unreachable from requester client")
        print("[python_http_server_integration] endorsement ping ok", file=sys.stderr)

        await request_endorsement_or_throw(
            requester_client,
            info["endorsement_service_node_id"],
            ETYPE_ACCESS,
            EVALUE_ACCESS_NETWORK,
            "network access",
        )
        await request_endorsement_or_throw(
            requester_client,
            info["endorsement_service_node_id"],
            ETYPE_ROLE,
            EVALUE_ROLE_CLIENT,
            "client role",
        )
        await request_endorsement_or_throw(
            requester_client,
            info["endorsement_service_node_id"],
            ETYPE_ROLE,
            EVALUE_ROLE_RESOURCE_SERVICE,
            "resource service role",
        )

        socket = await with_timeout(
            create_connection(requester_client, info["resource_service_node_id"], host_port),
            6.0,
            "create rsp connection",
        )
        print("[python_http_server_integration] rsp socket connected", file=sys.stderr)

        request_bytes = (
            b"GET / HTTP/1.1\r\n"
            b"Host: python-rsp.local\r\n"
            b"Connection: close\r\n\r\n"
        )
        await with_timeout(socket.write(request_bytes), 4.0, "http request write")

        response = await with_timeout(
            read_until_expected(socket, ["HTTP/1.1 200 OK", "Remote Socket Protocol"], 6.0),
            7.0,
            "http response read",
        )
        await socket.close()
        print("[python_http_server_integration] response received", file=sys.stderr)

        assert "HTTP/1.1 200 OK" in response
        print("python_http_server_integration passed")
    finally:
        print("[python_http_server_integration] cleanup begin", file=sys.stderr)
        if requester_client is not None:
            await requester_client.close()
        if server_proc is not None:
            await terminate_process(server_proc)
        if server_stderr_task is not None:
            server_stderr_task.cancel()
            await asyncio.gather(server_stderr_task, return_exceptions=True)
        await terminate_process(fixture)
        fixture_stderr_task.cancel()
        await asyncio.gather(fixture_stderr_task, return_exceptions=True)
        print("[python_http_server_integration] cleanup done", file=sys.stderr)


if __name__ == "__main__":
    async def _entrypoint() -> None:
        await asyncio.wait_for(main(), timeout=HARNESS_TIMEOUT_S)

    try:
        asyncio.run(_entrypoint())
    except asyncio.TimeoutError:
        print(f"python_http_server_integration failed: harness timed out after {HARNESS_TIMEOUT_S}s", file=sys.stderr)
        raise SystemExit(1)
