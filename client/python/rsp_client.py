"""Async Python RSP client — mirrors client/nodejs/rsp_client.js."""

import asyncio
import base64
import hashlib
import json
import os
import struct
import sys
import uuid
from typing import Any

from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives import hashes, serialization

import messages as _messages

JSON_FRAME_MAGIC = 0x5253504A
HANDSHAKE_TERMINATOR = b"\r\n\r\n"
P256_ALGORITHM = 100
DEFAULT_TIMEOUT_S = 5.0

SUCCESS = _messages.SOCKET_STATUS.SUCCESS
CONNECT_REFUSED = _messages.SOCKET_STATUS.CONNECT_REFUSED
CONNECT_TIMEOUT = _messages.SOCKET_STATUS.CONNECT_TIMEOUT
SOCKET_CLOSED = _messages.SOCKET_STATUS.SOCKET_CLOSED
SOCKET_DATA = _messages.SOCKET_STATUS.SOCKET_DATA
SOCKET_ERROR = _messages.SOCKET_STATUS.SOCKET_ERROR
NEW_CONNECTION = _messages.SOCKET_STATUS.NEW_CONNECTION
ASYNC_SOCKET = _messages.SOCKET_STATUS.ASYNC_SOCKET
INVALID_FLAGS = _messages.SOCKET_STATUS.INVALID_FLAGS
SOCKET_IN_USE = _messages.SOCKET_STATUS.SOCKET_IN_USE

ENDORSEMENT_SUCCESS = _messages.ENSDORSMENT_STATUS.ENDORSEMENT_SUCCESS
ENDORSEMENT_UNKNOWN_IDENTITY = _messages.ENSDORSMENT_STATUS.ENDORSEMENT_UNKNOWN_IDENTITY


# ---------------------------------------------------------------------------
# GUID / NodeId helpers
# ---------------------------------------------------------------------------

def _normalize_guid(value: str) -> str:
    compact = value.replace("{", "").replace("}", "").replace("-", "").lower()
    if len(compact) != 32 or not all(c in "0123456789abcdef" for c in compact):
        raise ValueError(f"invalid GUID/NodeID: {value}")
    return compact


def _guid_from_bytes(b: bytes) -> str:
    h = b.hex()
    return f"{h[0:8]}-{h[8:12]}-{h[12:16]}-{h[16:20]}-{h[20:32]}"


def _encode_node_id_for_field(node_id: str) -> str:
    """Native-endian encoding for destination/source fields."""
    normalized = _normalize_guid(node_id)
    high = int(normalized[:16], 16)
    low = int(normalized[16:], 16)
    if sys.byteorder == "little":
        data = struct.pack("<QQ", high, low)
    else:
        data = struct.pack(">QQ", high, low)
    return base64.b64encode(data).decode("ascii")


def _decode_node_id_field(b64_value: str) -> str:
    """Decode native-endian node id field back to standard UUID."""
    b = base64.b64decode(b64_value)
    if sys.byteorder == "little":
        high, low = struct.unpack("<QQ", b)
    else:
        high, low = struct.unpack(">QQ", b)
    canonical = struct.pack(">QQ", high, low)
    return _guid_from_bytes(canonical)


def _encode_node_id_for_signer(node_id: str) -> str:
    """Always big-endian for SignatureBlock.signer field."""
    normalized = _normalize_guid(node_id)
    high = int(normalized[:16], 16)
    low = int(normalized[16:], 16)
    return base64.b64encode(struct.pack(">QQ", high, low)).decode("ascii")


def _decode_signer_node_id(b64_value: str) -> str:
    return _guid_from_bytes(base64.b64decode(b64_value))


def _random_uuid_hex() -> str:
    return uuid.uuid4().hex


def _node_id_from_public_key(public_key) -> str:
    spki_der = public_key.public_bytes(
        serialization.Encoding.DER,
        serialization.PublicFormat.SubjectPublicKeyInfo,
    )
    digest = hashlib.sha256(spki_der).digest()[:16]
    return _guid_from_bytes(digest)


# ---------------------------------------------------------------------------
# Signing helpers
# ---------------------------------------------------------------------------

def _sign_rsp_message(private_key, local_node_id: str, message: dict) -> dict:
    digest = _messages.hash_rsp_message(message)
    sig_bytes = private_key.sign(digest, ec.ECDSA(hashes.SHA256()))
    return {
        "signer": {"value": _encode_node_id_for_signer(local_node_id)},
        "algorithm": P256_ALGORITHM,
        "signature": base64.b64encode(sig_bytes).decode("ascii"),
    }


def _verify_rsp_message_signature(public_key, message: dict, signature_block: dict) -> bool:
    if not signature_block or signature_block.get("algorithm") != P256_ALGORITHM:
        return False
    try:
        signer_node_id = _decode_signer_node_id(signature_block["signer"]["value"])
        expected_node_id = _node_id_from_public_key(public_key)
        if signer_node_id != expected_node_id:
            return False
        digest = _messages.hash_rsp_message(message)
        sig_bytes = base64.b64decode(signature_block["signature"])
        public_key.verify(sig_bytes, digest, ec.ECDSA(hashes.SHA256()))
        return True
    except Exception:
        return False


def _sign_endorsement(private_key, local_node_id: str, endorsement: dict) -> str:
    _unused = local_node_id
    unsigned_bytes = _serialize_unsigned_endorsement(endorsement)
    sig_bytes = private_key.sign(unsigned_bytes, ec.ECDSA(hashes.SHA256()))
    return base64.b64encode(sig_bytes).decode("ascii")


def _encode_varint_unsigned(value: int) -> bytes:
    if value < 0:
        raise ValueError("varint value must be non-negative")
    out = bytearray()
    remaining = int(value)
    while True:
        byte = remaining & 0x7F
        remaining >>= 7
        if remaining:
            byte |= 0x80
        out.append(byte)
        if not remaining:
            break
    return bytes(out)


def _encode_tag(field_number: int, wire_type: int) -> bytes:
    return _encode_varint_unsigned((field_number << 3) | wire_type)


def _encode_length_delimited_field(field_number: int, payload: bytes) -> bytes:
    return _encode_tag(field_number, 2) + _encode_varint_unsigned(len(payload)) + payload


def _serialize_uuid_like(b64_value: str) -> bytes:
    return _encode_length_delimited_field(1, base64.b64decode(b64_value))


def _serialize_date_time_message(milliseconds_since_epoch: int) -> bytes:
    return _encode_tag(1, 0) + _encode_varint_unsigned(milliseconds_since_epoch)


def _serialize_unsigned_endorsement(endorsement: dict) -> bytes:
    parts = [
        _encode_length_delimited_field(1, _serialize_uuid_like(endorsement["subject"]["value"])),
        _encode_length_delimited_field(2, _serialize_uuid_like(endorsement["endorsement_service"]["value"])),
        _encode_length_delimited_field(3, _serialize_uuid_like(endorsement["endorsement_type"]["value"])),
        _encode_length_delimited_field(4, base64.b64decode(endorsement.get("endorsement_value") or "")),
        _encode_length_delimited_field(
            5,
            _serialize_date_time_message(int(endorsement["valid_until"]["milliseconds_since_epoch"])),
        ),
    ]
    return b"".join(parts)


def _public_key_pem_bytes(public_key) -> bytes:
    return public_key.public_bytes(
        serialization.Encoding.PEM,
        serialization.PublicFormat.SubjectPublicKeyInfo,
    )


def _load_public_key_from_b64(b64_pem: str):
    pem_bytes = base64.b64decode(b64_pem)
    return serialization.load_pem_public_key(pem_bytes)


# ---------------------------------------------------------------------------
# Transport helpers
# ---------------------------------------------------------------------------

def _parse_transport_spec(transport_spec: str):
    sep = transport_spec.index(":")
    transport_name = transport_spec[:sep]
    parameters = transport_spec[sep + 1:]
    if transport_name != "tcp":
        raise ValueError("the Python client currently supports only tcp transport")
    last_colon = parameters.rfind(":")
    host = parameters[:last_colon]
    port = int(parameters[last_colon + 1:])
    return host, port


def _has_field(obj: Any, key: str) -> bool:
    return (
        obj is not None
        and isinstance(obj, dict)
        and key in obj
        and obj[key] is not None
    )


SERVICE_MESSAGE_TYPE_PREFIX = "type.rsp/rsp.proto."


def _pack_service_message(type_name: str, fields: dict) -> dict:
    return {"@type": SERVICE_MESSAGE_TYPE_PREFIX + type_name, **fields}


def _service_message_type_name(msg: dict) -> str | None:
    if not _has_field(msg, "service_message") or not msg["service_message"].get("@type"):
        return None
    type_url = msg["service_message"]["@type"]
    slash = type_url.rfind("/")
    return type_url[slash + 1:] if slash >= 0 else type_url


def _unpack_service_message(msg: dict) -> dict | None:
    if not _has_field(msg, "service_message"):
        return None
    copy = dict(msg["service_message"])
    copy.pop("@type", None)
    return copy


# ---------------------------------------------------------------------------
# RSPClient
# ---------------------------------------------------------------------------

class RSPClient:
    def __init__(self, key_pair=None):
        if key_pair is not None:
            self._private_key, self._public_key = key_pair
        else:
            self._private_key = ec.generate_private_key(ec.SECP256R1())
            self._public_key = self._private_key.public_key()

        self.node_id: str = _node_id_from_public_key(self._public_key)

        self._reader: asyncio.StreamReader | None = None
        self._writer: asyncio.StreamWriter | None = None
        self._stopping = False
        self._receive_task: asyncio.Task | None = None

        self.peer_node_id: str | None = None
        self.peer_public_key = None

        self._ping_sequence = 1
        self._pending_pings: dict[str, dict] = {}        # nonce -> {sequence, future}

        self._pending_connects: dict[str, asyncio.Future] = {}
        self._pending_listens: dict[str, asyncio.Future] = {}
        self._socket_routes: dict[str, str] = {}         # socketId -> nodeId
        self._socket_reply_queues: dict[str, list] = {}  # socketId -> [SocketReply]
        self._awaited_socket_replies: dict[str, asyncio.Future] = {}
        self._pending_socket_replies: list = []

        self._pending_endorsements: dict[str, asyncio.Future] = {}  # nodeIdHex -> future
        self._endorsement_cache: dict[str, dict] = {}
        self._identity_cache: dict[str, Any] = {}

        self._socket_handlers: dict[str, Any] = {}      # socketId -> handler
        self._pending_resource_advertisements: list = []
        self._pending_resource_list: asyncio.Future | None = None

    # --- Context manager ---

    async def __aenter__(self):
        return self

    async def __aexit__(self, *args):
        await self.close()

    # --- Connection lifecycle ---

    async def connect(self, transport_spec: str) -> None:
        host, port = _parse_transport_spec(transport_spec)
        self._reader, self._writer = await asyncio.open_connection(host, port)

        # ASCII handshake: read banner, send encoding choice, read confirmation
        await self._read_until(HANDSHAKE_TERMINATOR)
        self._writer.write(b"encoding:json\r\n\r\n")
        await self._writer.drain()
        result = await self._read_until(HANDSHAKE_TERMINATOR)
        if not result.startswith(b"1success: encoding:json"):
            raise ConnectionError(f"ASCII handshake failed: {result.strip()}")

        await self._perform_initial_identity_exchange()

        self._stopping = False
        self._receive_task = asyncio.get_event_loop().create_task(self._receive_loop())

    async def close(self) -> None:
        if self._writer is None:
            return

        self._stopping = True
        writer = self._writer
        self._writer = None
        self._reader = None
        self.peer_node_id = None
        self.peer_public_key = None

        closed_error = ConnectionError("client closed")

        for fut in self._pending_pings.values():
            if not fut["future"].done():
                fut["future"].set_exception(closed_error)
        self._pending_pings.clear()

        for fut in self._pending_connects.values():
            if not fut.done():
                fut.set_exception(closed_error)
        self._pending_connects.clear()

        for fut in self._pending_listens.values():
            if not fut.done():
                fut.set_exception(closed_error)
        self._pending_listens.clear()

        for fut in self._awaited_socket_replies.values():
            if not fut.done():
                fut.set_exception(closed_error)
        self._awaited_socket_replies.clear()

        for fut in self._pending_endorsements.values():
            if not fut.done():
                fut.set_exception(closed_error)
        self._pending_endorsements.clear()

        self._socket_handlers.clear()

        try:
            writer.close()
            await writer.wait_closed()
        except Exception:
            pass

        if self._receive_task:
            try:
                await asyncio.wait_for(asyncio.shield(self._receive_task), timeout=2.0)
            except Exception:
                self._receive_task.cancel()
            self._receive_task = None

    # --- Identity exchange ---

    async def _perform_initial_identity_exchange(self) -> None:
        local_challenge_nonce = _random_uuid_hex()
        await self._send_raw_message({"challenge_request": {"nonce": {"value": local_challenge_nonce}}})

        peer_challenge_received = False
        peer_identity_received = False

        while not peer_challenge_received or not peer_identity_received:
            message = await self._receive_raw_message()

            if _has_field(message, "challenge_request"):
                nonce_hex = (message.get("challenge_request") or {}).get("nonce", {}).get("value")
                if (message.get("destination") or message.get("signature") or
                        peer_challenge_received or not nonce_hex or len(nonce_hex) != 32):
                    raise ConnectionError("received an invalid challenge request during authentication")
                pub_pem_b64 = base64.b64encode(_public_key_pem_bytes(self._public_key)).decode("ascii")
                identity_message = {
                    "identity": {
                        "nonce": {"value": nonce_hex},
                        "public_key": {
                            "algorithm": P256_ALGORITHM,
                            "public_key": pub_pem_b64,
                        },
                    },
                }
                identity_message["signature"] = _sign_rsp_message(
                    self._private_key, self.node_id, identity_message
                )
                await self._send_raw_message(identity_message)
                peer_challenge_received = True
                continue

            if _has_field(message, "identity"):
                peer_public_key_b64 = (
                    message.get("identity", {}).get("public_key", {}).get("public_key", "")
                )
                peer_public_key = _load_public_key_from_b64(peer_public_key_b64)
                nonce_in_identity = (
                    (message.get("identity") or {}).get("nonce", {}).get("value")
                )
                if (nonce_in_identity != local_challenge_nonce or
                        not _verify_rsp_message_signature(peer_public_key, message, message.get("signature"))):
                    raise ConnectionError("received an invalid identity response during authentication")
                self.peer_public_key = peer_public_key
                self.peer_node_id = _node_id_from_public_key(peer_public_key)
                self._identity_cache[self.peer_node_id] = peer_public_key
                peer_identity_received = True
                continue

            raise ConnectionError("received an unexpected message during authentication")

    async def _send_identity_to(self, node_id: str) -> None:
        pub_pem_b64 = base64.b64encode(_public_key_pem_bytes(self._public_key)).decode("ascii")
        identity_message = {
            "destination": {"value": _encode_node_id_for_field(node_id)},
            "identities": [{
                "public_key": {
                    "algorithm": P256_ALGORITHM,
                    "public_key": pub_pem_b64,
                },
            }],
        }
        identity_message["signature"] = _sign_rsp_message(self._private_key, self.node_id, identity_message)
        await self._send_raw_message(identity_message)

    # --- Receive loop ---

    async def _receive_loop(self) -> None:
        while not self._stopping and self._reader is not None:
            try:
                message = await self._receive_raw_message()
            except Exception:
                if not self._stopping:
                    raise
                return
            try:
                self._dispatch_message(message)
            except Exception:
                pass  # swallow dispatch errors

    def _dispatch_message(self, msg: dict) -> None:
        if isinstance(msg.get("endorsements"), list):
            for endorsement in msg["endorsements"]:
                self._cache_endorsement(endorsement)

        if isinstance(msg.get("identities"), list):
            for identity in msg["identities"]:
                self._cache_identity(identity)

        if _has_field(msg, "ping_reply"):
            self._handle_ping_reply(msg)
        elif _has_field(msg, "service_message"):
            type_name = _service_message_type_name(msg)
            if type_name == "rsp.proto.SocketReply":
                self._handle_socket_reply(msg, _unpack_service_message(msg))
            elif type_name == "rsp.proto.EndorsementDone":
                self._handle_endorsement_done(msg)
        elif _has_field(msg, "endorsement_needed"):
            pass  # endorsement_needed not yet handled
        elif _has_field(msg, "resource_advertisement"):
            if self._pending_resource_list is not None and not self._pending_resource_list.done():
                self._pending_resource_list.set_result(msg["resource_advertisement"])
                self._pending_resource_list = None
            else:
                self._pending_resource_advertisements.append(msg["resource_advertisement"])

    def _handle_ping_reply(self, msg: dict) -> None:
        reply = msg["ping_reply"]
        nonce = (reply.get("nonce") or {}).get("value")
        if not nonce:
            return
        pending = self._pending_pings.get(nonce)
        if not pending or reply.get("sequence") != pending["sequence"]:
            return
        fut = pending["future"]
        self._pending_pings.pop(nonce, None)
        if not fut.done():
            fut.set_result(True)

    def _handle_socket_reply(self, msg: dict, socket_reply: dict) -> None:
        socket_id_hex = (socket_reply.get("socket_id") or {}).get("value")

        if socket_id_hex:
            connect_fut = self._pending_connects.get(socket_id_hex)
            if connect_fut:
                status = socket_reply.get("error") or 0
                if status in (SUCCESS, CONNECT_REFUSED, CONNECT_TIMEOUT,
                              SOCKET_ERROR, SOCKET_IN_USE, INVALID_FLAGS):
                    self._pending_connects.pop(socket_id_hex, None)
                    if not connect_fut.done():
                        connect_fut.set_result(socket_reply)
                    return

            listen_fut = self._pending_listens.get(socket_id_hex)
            if listen_fut:
                status = socket_reply.get("error") or 0
                if status in (SUCCESS, SOCKET_ERROR, SOCKET_IN_USE, INVALID_FLAGS):
                    self._pending_listens.pop(socket_id_hex, None)
                    if not listen_fut.done():
                        listen_fut.set_result(socket_reply)
                    return

            new_socket_id = (socket_reply.get("new_socket_id") or {}).get("value")
            if new_socket_id:
                source_node_id = self._decode_source_node_id(msg)
                if not source_node_id:
                    source_node_id = self._socket_routes.get(socket_id_hex)
                if source_node_id:
                    self._socket_routes[_normalize_guid(new_socket_id)] = source_node_id

            handler = self._socket_handlers.get(socket_id_hex)
            if handler:
                handler(socket_reply)
                return

            queue = self._socket_reply_queues.setdefault(socket_id_hex, [])
            queue.append(socket_reply)

            awaited = self._awaited_socket_replies.get(socket_id_hex)
            if awaited:
                self._awaited_socket_replies.pop(socket_id_hex, None)
                queue.pop()
                if not queue:
                    self._socket_reply_queues.pop(socket_id_hex, None)
                if not awaited.done():
                    awaited.set_result(socket_reply)
                return

        self._pending_socket_replies.append(socket_reply)

    def _handle_endorsement_done(self, msg: dict) -> None:
        source_node_id = self._decode_source_node_id(msg)
        if not source_node_id:
            return
        pending_key = _encode_node_id_for_field(source_node_id)
        fut = self._pending_endorsements.get(pending_key)
        if not fut:
            return
        self._pending_endorsements.pop(pending_key, None)
        if not fut.done():
            fut.set_result(_unpack_service_message(msg))

    def _decode_source_node_id(self, msg: dict) -> str | None:
        source_value = (msg.get("source") or {}).get("value")
        if not source_value:
            return None
        try:
            return _decode_node_id_field(source_value)
        except Exception:
            return None

    def _cache_endorsement(self, endorsement: dict) -> None:
        subject_hex = (endorsement.get("subject") or {}).get("value")
        type_hex = (endorsement.get("endorsement_type") or {}).get("value")
        if not subject_hex or not type_hex:
            return
        try:
            subject = _decode_node_id_field(subject_hex)
            self._endorsement_cache[f"{subject}:{type_hex}"] = endorsement
        except Exception:
            pass

    def _cache_identity(self, identity: dict) -> None:
        pub_key_b64 = (identity.get("public_key") or {}).get("public_key")
        if not pub_key_b64:
            return
        try:
            pub_key = _load_public_key_from_b64(pub_key_b64)
            node_id = _node_id_from_public_key(pub_key)
            self._identity_cache[node_id] = pub_key
        except Exception:
            pass

    # --- Message send/receive primitives ---

    async def _send_raw_message(self, message: dict) -> None:
        if self._writer is None:
            raise ConnectionError("client is not connected")
        payload = json.dumps(message).encode("utf-8")
        header = struct.pack(">II", JSON_FRAME_MAGIC, len(payload))
        self._writer.write(header + payload)
        await self._writer.drain()

    async def _receive_raw_message(self) -> dict:
        if self._reader is None:
            raise ConnectionError("client is not connected")
        header = await self._reader.readexactly(8)
        magic, payload_length = struct.unpack(">II", header)
        if magic != JSON_FRAME_MAGIC:
            raise ValueError(f"unexpected JSON frame magic: 0x{magic:08x}")
        payload = await self._reader.readexactly(payload_length)
        return json.loads(payload.decode("utf-8"))

    async def _read_until(self, marker: bytes) -> bytes:
        """Read from the raw stream until the marker bytes are found."""
        buf = b""
        while True:
            chunk = await self._reader.read(1)
            if not chunk:
                raise ConnectionError("connection closed during handshake")
            buf += chunk
            if buf.endswith(marker):
                return buf

    async def _send_signed_message(self, message: dict) -> None:
        signed = dict(message)
        signed["signature"] = _sign_rsp_message(self._private_key, self.node_id, message)
        await self._send_raw_message(signed)

    # --- Socket reply waiting ---

    async def _wait_for_socket_reply(
        self, socket_id: str, timeout_s: float = DEFAULT_TIMEOUT_S
    ) -> dict:
        queue = self._socket_reply_queues.get(socket_id)
        if queue:
            reply = queue.pop(0)
            if not queue:
                self._socket_reply_queues.pop(socket_id, None)
            if reply in self._pending_socket_replies:
                self._pending_socket_replies.remove(reply)
            return reply

        loop = asyncio.get_event_loop()
        fut: asyncio.Future = loop.create_future()
        self._awaited_socket_replies[socket_id] = fut
        try:
            return await asyncio.wait_for(asyncio.shield(fut), timeout=timeout_s)
        except asyncio.TimeoutError:
            self._awaited_socket_replies.pop(socket_id, None)
            raise TimeoutError(f"socket reply timed out for socket {socket_id}")

    # --- Ping ---

    async def ping(self, node_id: str, timeout: float = DEFAULT_TIMEOUT_S) -> bool:
        ping_nonce = _random_uuid_hex()
        sequence = self._ping_sequence
        self._ping_sequence += 1

        request = {
            "destination": {"value": _encode_node_id_for_field(node_id)},
            "nonce": {"value": _random_uuid_hex()},
            "ping_request": {
                "nonce": {"value": ping_nonce},
                "sequence": sequence,
                "time_sent": {"milliseconds_since_epoch": int(__import__("time").time() * 1000)},
            },
        }

        loop = asyncio.get_event_loop()
        fut: asyncio.Future = loop.create_future()
        self._pending_pings[ping_nonce] = {"sequence": sequence, "future": fut}

        try:
            await self._send_signed_message(request)
        except Exception:
            self._pending_pings.pop(ping_nonce, None)
            return False

        try:
            return await asyncio.wait_for(asyncio.shield(fut), timeout=timeout)
        except asyncio.TimeoutError:
            self._pending_pings.pop(ping_nonce, None)
            return False

    # --- Connect ---

    async def connect_tcp_ex(
        self, node_id: str, host_port: str, *,
        timeout_ms: int = 0, retries: int = 0, retry_ms: int = 0,
        async_data: bool = False, share_socket: bool = False, use_socket: bool = False,
    ) -> dict | None:
        socket_id = _random_uuid_hex()
        tcp: dict = {
            "host_port": host_port,
            "socket_number": {"value": socket_id},
        }
        if use_socket:
            tcp["use_socket"] = True
        if timeout_ms > 0:
            tcp["timeout_ms"] = timeout_ms
        if retries > 0:
            tcp["retries"] = retries
        if retry_ms > 0:
            tcp["retry_ms"] = retry_ms
        if async_data:
            tcp["async_data"] = True
        if share_socket:
            tcp["share_socket"] = True

        request: dict = {
            "destination": {"value": _encode_node_id_for_field(node_id)},
            "service_message": _pack_service_message("ConnectTCPRequest", tcp),
        }

        wait_s = (timeout_ms / 1000.0 + 1.0) if timeout_ms > 0 else DEFAULT_TIMEOUT_S

        loop = asyncio.get_event_loop()
        fut: asyncio.Future = loop.create_future()
        self._pending_connects[socket_id] = fut

        try:
            await self._send_signed_message(request)
        except Exception:
            self._pending_connects.pop(socket_id, None)
            return None

        try:
            reply = await asyncio.wait_for(asyncio.shield(fut), timeout=wait_s)
        except (asyncio.TimeoutError, TimeoutError):
            self._pending_connects.pop(socket_id, None)
            return None

        if reply and (reply.get("error") or 0) == SUCCESS:
            confirmed_id = (reply.get("socket_id") or {}).get("value") or socket_id
            if not (reply.get("socket_id") or {}).get("value"):
                reply.setdefault("socket_id", {})["value"] = socket_id
            self._socket_routes[confirmed_id] = node_id
        return reply

    async def connect_tcp(self, node_id: str, host_port: str, **options) -> str | None:
        try:
            reply = await self.connect_tcp_ex(node_id, host_port, **options)
        except Exception:
            return None
        if not reply or (reply.get("error") or 0) != SUCCESS:
            return None
        return (reply.get("socket_id") or {}).get("value")

    # --- Listen ---

    async def listen_tcp_ex(
        self, node_id: str, host_port: str, *,
        timeout_ms: int = 0, async_accept: bool = False,
        share_listening_socket: bool = False, share_child_sockets: bool = False,
        children_use_socket: bool = False, children_async_data: bool = False,
    ) -> dict | None:
        socket_id = _random_uuid_hex()
        tcp: dict = {
            "host_port": host_port,
            "socket_number": {"value": socket_id},
        }
        if timeout_ms > 0:
            tcp["timeout_ms"] = timeout_ms
        if async_accept:
            tcp["async_accept"] = True
        if share_listening_socket:
            tcp["share_listening_socket"] = True
        if share_child_sockets:
            tcp["share_child_sockets"] = True
        if children_use_socket:
            tcp["children_use_socket"] = True
        if children_async_data:
            tcp["children_async_data"] = True

        request: dict = {
            "destination": {"value": _encode_node_id_for_field(node_id)},
            "service_message": _pack_service_message("ListenTCPRequest", tcp),
        }

        wait_s = (timeout_ms / 1000.0 + 1.0) if timeout_ms > 0 else DEFAULT_TIMEOUT_S

        loop = asyncio.get_event_loop()
        fut: asyncio.Future = loop.create_future()
        self._pending_listens[socket_id] = fut

        try:
            await self._send_signed_message(request)
        except Exception:
            self._pending_listens.pop(socket_id, None)
            return None

        try:
            reply = await asyncio.wait_for(asyncio.shield(fut), timeout=wait_s)
        except (asyncio.TimeoutError, TimeoutError):
            self._pending_listens.pop(socket_id, None)
            return None

        if reply and (reply.get("error") or 0) == SUCCESS:
            confirmed_id = (reply.get("socket_id") or {}).get("value") or socket_id
            if not (reply.get("socket_id") or {}).get("value"):
                reply.setdefault("socket_id", {})["value"] = socket_id
            self._socket_routes[confirmed_id] = node_id
        return reply

    async def listen_tcp(self, node_id: str, host_port: str, **options) -> str | None:
        try:
            reply = await self.listen_tcp_ex(node_id, host_port, **options)
        except Exception:
            return None
        if not reply or (reply.get("error") or 0) != SUCCESS:
            return None
        return (reply.get("socket_id") or {}).get("value")

    # --- Accept ---

    async def accept_tcp_ex(
        self, listen_socket_id: str, *,
        new_socket_id: str | None = None, timeout_ms: int = 0,
        share_child_socket: bool = False, child_use_socket: bool = False,
        child_async_data: bool = False,
    ) -> dict | None:
        node_id = self._socket_routes.get(listen_socket_id)
        if not node_id:
            return None

        request: dict = {
            "destination": {"value": _encode_node_id_for_field(node_id)},
            "service_message": _pack_service_message("AcceptTCP", {"listen_socket_number": {"value": listen_socket_id}}),
        }
        tcp = request["service_message"]
        if new_socket_id:
            tcp["new_socket_number"] = {"value": _normalize_guid(new_socket_id)}
        if timeout_ms > 0:
            tcp["timeout_ms"] = timeout_ms
        if share_child_socket:
            tcp["share_child_socket"] = True
        if child_use_socket:
            tcp["child_use_socket"] = True
        if child_async_data:
            tcp["child_async_data"] = True

        try:
            await self._send_signed_message(request)
        except Exception:
            return None

        wait_s = (timeout_ms / 1000.0 + 1.0) if timeout_ms > 0 else DEFAULT_TIMEOUT_S
        try:
            reply = await self._wait_for_socket_reply(listen_socket_id, wait_s)
        except Exception:
            return None

        if reply and (reply.get("error") in (SUCCESS, NEW_CONNECTION)):
            new_sid = (reply.get("new_socket_id") or {}).get("value")
            if new_sid:
                self._socket_routes[new_sid] = node_id
        return reply

    async def accept_tcp(self, listen_socket_id: str, **options) -> str | None:
        try:
            reply = await self.accept_tcp_ex(listen_socket_id, **options)
        except Exception:
            return None
        if not reply or reply.get("error") not in (SUCCESS, NEW_CONNECTION):
            return None
        return (reply.get("new_socket_id") or {}).get("value")

    # --- Socket send / recv / close ---

    async def socket_send(self, socket_id: str, data: bytes) -> bool:
        lookup_socket_id = _normalize_guid(socket_id)
        node_id = self._socket_routes.get(lookup_socket_id) or self._socket_routes.get(socket_id)
        if not node_id:
            return False
        data_b64 = base64.b64encode(data if isinstance(data, (bytes, bytearray)) else bytes(data)).decode("ascii")
        request = {
            "destination": {"value": _encode_node_id_for_field(node_id)},
            "service_message": _pack_service_message("SocketSend", {"socket_number": {"value": socket_id}, "data": data_b64}),
        }
        try:
            await self._send_signed_message(request)
        except Exception:
            return False

        import time
        deadline = time.monotonic() + DEFAULT_TIMEOUT_S
        while time.monotonic() < deadline:
            remaining = deadline - time.monotonic()
            try:
                reply = await self._wait_for_socket_reply(socket_id, remaining)
            except Exception:
                return False

            status = (reply.get("error") if reply else None) or 0
            if status == SUCCESS:
                return True
            if status in (SOCKET_DATA, NEW_CONNECTION, ASYNC_SOCKET):
                continue
            if status in (SOCKET_CLOSED, SOCKET_ERROR, INVALID_FLAGS):
                return False

        return False

    async def socket_recv_ex(
        self, socket_id: str, max_bytes: int = 4096, wait_ms: int = 0
    ) -> dict | None:
        node_id = self._socket_routes.get(socket_id)
        if not node_id:
            return None
        request: dict = {
            "destination": {"value": _encode_node_id_for_field(node_id)},
            "service_message": _pack_service_message("SocketRecv", {"socket_number": {"value": socket_id}, "max_bytes": max_bytes}),
        }
        if wait_ms > 0:
            request["service_message"]["wait_ms"] = wait_ms
        try:
            await self._send_signed_message(request)
        except Exception:
            return None
        reply_timeout_s = (wait_ms / 1000.0 + 1.0) if wait_ms > 0 else DEFAULT_TIMEOUT_S
        try:
            return await self._wait_for_socket_reply(socket_id, reply_timeout_s)
        except Exception:
            return None

    async def socket_recv(
        self, socket_id: str, max_bytes: int = 4096, wait_ms: int = 0
    ) -> bytes | None:
        reply = await self.socket_recv_ex(socket_id, max_bytes, wait_ms)
        if not reply or reply.get("error") not in (SOCKET_DATA, SUCCESS):
            return None
        data_b64 = reply.get("data") or ""
        return base64.b64decode(data_b64) if data_b64 else b""

    async def socket_close(self, socket_id: str) -> bool:
        node_id = self._socket_routes.get(socket_id)
        if not node_id:
            return False
        request = {
            "destination": {"value": _encode_node_id_for_field(node_id)},
            "service_message": _pack_service_message("SocketClose", {"socket_number": {"value": socket_id}}),
        }
        try:
            await self._send_signed_message(request)
        except Exception:
            self._socket_routes.pop(socket_id, None)
            return True

        import time
        deadline = time.monotonic() + DEFAULT_TIMEOUT_S
        while time.monotonic() < deadline:
            remaining = deadline - time.monotonic()
            try:
                reply = await self._wait_for_socket_reply(socket_id, remaining)
            except Exception:
                break
            if not reply:
                break
            if reply.get("error") in (SUCCESS, SOCKET_CLOSED):
                self._socket_routes.pop(socket_id, None)
                return True
            if reply.get("error") not in (SOCKET_DATA, ASYNC_SOCKET, NEW_CONNECTION):
                return False

        self._socket_routes.pop(socket_id, None)
        return True

    # --- Non-blocking dequeue ---

    def try_dequeue_socket_reply(self) -> dict | None:
        return self._pending_socket_replies.pop(0) if self._pending_socket_replies else None

    def pending_socket_reply_count(self) -> int:
        return len(self._pending_socket_replies)

    def try_dequeue_resource_advertisement(self) -> dict | None:
        return (
            self._pending_resource_advertisements.pop(0)
            if self._pending_resource_advertisements
            else None
        )

    def pending_resource_advertisement_count(self) -> int:
        return len(self._pending_resource_advertisements)

    # --- Route registration ---

    def register_socket_route(self, socket_id: str, node_id: str) -> None:
        self._socket_routes[_normalize_guid(socket_id)] = node_id

    def attach_socket_handler(self, socket_id: str, handler) -> None:
        self._socket_handlers[socket_id] = handler
        queue = self._socket_reply_queues.pop(socket_id, None)
        if queue:
            for reply in queue:
                if reply in self._pending_socket_replies:
                    self._pending_socket_replies.remove(reply)
            loop = asyncio.get_event_loop()
            for reply in queue:
                loop.call_soon(handler, reply)

    def detach_socket_handler(self, socket_id: str) -> None:
        self._socket_handlers.pop(socket_id, None)

    async def send_socket_data(self, socket_id: str, data: bytes) -> bool:
        """Fire-and-forget socket send (no SUCCESS reply wait)."""
        node_id = self._socket_routes.get(socket_id)
        if not node_id:
            return False
        data_b64 = base64.b64encode(data if isinstance(data, (bytes, bytearray)) else bytes(data)).decode("ascii")
        request = {
            "destination": {"value": _encode_node_id_for_field(node_id)},
            "service_message": _pack_service_message("SocketSend", {"socket_number": {"value": socket_id}, "data": data_b64}),
        }
        try:
            await self._send_signed_message(request)
            return True
        except Exception:
            return False

    # --- Endorsements ---

    async def begin_endorsement_request(
        self, node_id: str, endorsement_type: str, endorsement_value: bytes = b""
    ) -> dict | None:
        pending_key = _encode_node_id_for_field(node_id)
        if pending_key in self._pending_endorsements:
            return None

        if isinstance(endorsement_value, (bytes, bytearray)):
            endorsement_value_b64 = base64.b64encode(endorsement_value).decode("ascii")
        else:
            endorsement_value_b64 = base64.b64encode(bytes(endorsement_value)).decode("ascii")

        import time as _time
        requested = {
            "subject": {"value": _encode_node_id_for_signer(self.node_id)},
            "endorsement_service": {"value": _encode_node_id_for_signer(node_id)},
            "endorsement_type": {"value": _normalize_guid(endorsement_type)},
            "endorsement_value": endorsement_value_b64,
            "valid_until": {"milliseconds_since_epoch": int(_time.time() * 1000) + 86400000},
        }
        requested["signature"] = _sign_endorsement(self._private_key, self.node_id, requested)

        repaired_unknown_identity = False
        while True:
            try:
                done = await self._send_endorsement_request(node_id, pending_key, requested)
            except Exception:
                return None
            if done is None:
                return None

            if (not repaired_unknown_identity and
                    (done.get("status") or 0) == ENDORSEMENT_UNKNOWN_IDENTITY):
                repaired_unknown_identity = True
                try:
                    await self._send_identity_to(node_id)
                except Exception:
                    pass
                continue

            if done.get("status") == ENDORSEMENT_SUCCESS and done.get("new_endorsement"):
                self._cache_endorsement(done["new_endorsement"])
            return done

    async def _send_endorsement_request(
        self, node_id: str, pending_key: str, requested: dict
    ) -> dict | None:
        loop = asyncio.get_event_loop()
        fut: asyncio.Future = loop.create_future()
        self._pending_endorsements[pending_key] = fut

        request = {
            "destination": {"value": _encode_node_id_for_field(node_id)},
            "service_message": _pack_service_message("BeginEndorsementRequest", {"requested_values": requested}),
        }
        try:
            await self._send_signed_message(request)
        except Exception:
            self._pending_endorsements.pop(pending_key, None)
            raise

        try:
            return await asyncio.wait_for(asyncio.shield(fut), timeout=DEFAULT_TIMEOUT_S)
        except asyncio.TimeoutError:
            self._pending_endorsements.pop(pending_key, None)
            return None

    # --- Resource query ---

    async def query_resources(
        self, node_id: str, query: str = "", max_records: int = 0
    ) -> bool:
        request: dict = {
            "destination": {"value": _encode_node_id_for_field(node_id)},
            "resource_query": {},
        }
        if query:
            request["resource_query"]["query"] = query
        if max_records > 0:
            request["resource_query"]["max_records"] = max_records
        try:
            await self._send_signed_message(request)
            return True
        except Exception:
            return False

    async def resource_list(
        self, node_id: str, query: str = "", max_records: int = 0,
        timeout: float = DEFAULT_TIMEOUT_S,
    ) -> dict | None:
        loop = asyncio.get_event_loop()
        fut: asyncio.Future = loop.create_future()
        self._pending_resource_list = fut

        try:
            await self._send_signed_message({
                "destination": {"value": _encode_node_id_for_field(node_id)},
                "resource_query": {
                    **(({"query": query} if query else {})),
                    **(({"max_records": max_records} if max_records > 0 else {})),
                },
            })
        except Exception:
            self._pending_resource_list = None
            return None

        try:
            return await asyncio.wait_for(asyncio.shield(fut), timeout=timeout)
        except asyncio.TimeoutError:
            self._pending_resource_list = None
            return None


# --- Public re-exports for callers that import from this module ---
decode_node_id_field = _decode_node_id_field
encode_node_id_for_field = _encode_node_id_for_field
encode_node_id_for_signer = _encode_node_id_for_signer
node_id_from_public_key = _node_id_from_public_key
