"""Async RSP socket abstraction — Python equivalent of client/nodejs/rsp_net.js."""

import asyncio
from typing import Callable

from rsp_client import RSPClient, SUCCESS, SOCKET_DATA, SOCKET_CLOSED, SOCKET_ERROR, NEW_CONNECTION


class RSPSocket:
    """Wraps a single RSP socket ID. Use as an async context manager."""

    def __init__(self, client: RSPClient, socket_id: str) -> None:
        self._client = client
        self._socket_id = socket_id
        self._recv_queue: asyncio.Queue[bytes | None] = asyncio.Queue()
        self._closed = False
        client.attach_socket_handler(socket_id, self._on_socket_reply)

    @property
    def socket_id(self) -> str:
        return self._socket_id

    def _on_socket_reply(self, reply: dict) -> None:
        status = reply.get("error") or 0
        if status == SOCKET_DATA:
            data_hex = reply.get("data") or ""
            data = bytes.fromhex(data_hex) if data_hex else b""
            self._recv_queue.put_nowait(data)
        elif status in (SOCKET_CLOSED, SOCKET_ERROR):
            self._closed = True
            self._client.detach_socket_handler(self._socket_id)
            self._recv_queue.put_nowait(None)

    async def read(self, n: int = -1) -> bytes:
        """Read the next data chunk. Returns b'' on EOF."""
        if self._closed and self._recv_queue.empty():
            return b""
        chunk = await self._recv_queue.get()
        return chunk if chunk is not None else b""

    async def write(self, data: bytes) -> None:
        """Send data using fire-and-forget send."""
        sent = await self._client.socket_send(self._socket_id, data)
        if not sent:
            raise ConnectionError(f"socket send failed for socket {self._socket_id}")

    async def close(self) -> None:
        if self._closed:
            return
        self._closed = True
        self._client.detach_socket_handler(self._socket_id)
        await self._client.socket_close(self._socket_id)

    async def __aenter__(self):
        return self

    async def __aexit__(self, *args):
        await self.close()

    def __aiter__(self):
        return self

    async def __anext__(self) -> bytes:
        data = await self.read()
        if not data:
            raise StopAsyncIteration
        return data


class RSPServer:
    """Async RSP server. Emits connections via accept() or async iteration."""

    def __init__(self, client: RSPClient, node_id: str, listen_socket_id: str) -> None:
        self._client = client
        self._node_id = node_id
        self._listen_socket_id = listen_socket_id
        self._closed = False
        self._accept_queue: asyncio.Queue[RSPSocket | None] = asyncio.Queue()
        client.attach_socket_handler(listen_socket_id, self._on_listen_reply)

    @property
    def socket_id(self) -> str:
        return self._listen_socket_id

    def _on_listen_reply(self, reply: dict) -> None:
        status = reply.get("error") or 0
        if status == NEW_CONNECTION:
            new_socket_id = (reply.get("new_socket_id") or {}).get("value")
            if new_socket_id:
                socket = RSPSocket(self._client, new_socket_id)
                self._accept_queue.put_nowait(socket)
        elif status in (SOCKET_CLOSED, SOCKET_ERROR):
            if not self._closed:
                self._closed = True
                self._client.detach_socket_handler(self._listen_socket_id)
                self._accept_queue.put_nowait(None)

    async def accept(self) -> "RSPSocket":
        """Wait for and return the next incoming RSPSocket."""
        if self._closed and self._accept_queue.empty():
            raise ConnectionError("server is closed")
        sock = await self._accept_queue.get()
        if sock is None:
            raise ConnectionError("server closed")
        return sock

    async def close(self) -> None:
        if self._closed:
            return
        self._closed = True
        self._client.detach_socket_handler(self._listen_socket_id)
        await self._client.socket_close(self._listen_socket_id)

    async def __aenter__(self):
        return self

    async def __aexit__(self, *args):
        await self.close()

    def __aiter__(self):
        return self

    async def __anext__(self) -> "RSPSocket":
        try:
            return await self.accept()
        except ConnectionError:
            raise StopAsyncIteration


async def create_connection(
    client: RSPClient, node_id: str, host_port: str, **options
) -> RSPSocket:
    """Connect to host_port via the RSP node and return an RSPSocket."""
    reply = await client.connect_tcp_ex(node_id, host_port, async_data=True, **options)
    status = (reply.get("error") if reply else None) or SUCCESS
    if status != SUCCESS or not (reply or {}).get("socket_id", {}).get("value"):
        raise ConnectionError(f"RSP connect failed (status={status})")
    return RSPSocket(client, reply["socket_id"]["value"])


async def create_server(
    client: RSPClient, node_id: str, host_port: str, **options
) -> RSPServer:
    """Listen on host_port via the RSP node and return an RSPServer."""
    reply = await client.listen_tcp_ex(
        node_id, host_port,
        async_accept=True,
        children_async_data=True,
        **options,
    )
    status = (reply.get("error") if reply else None) or SUCCESS
    if status != SUCCESS or not (reply or {}).get("socket_id", {}).get("value"):
        raise ConnectionError(f"RSP listen failed (status={status})")
    return RSPServer(client, node_id, reply["socket_id"]["value"])
