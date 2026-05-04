#!/usr/bin/env python3

import os
import sys


REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
if REPO_ROOT not in sys.path:
    sys.path.insert(0, REPO_ROOT)

from client.python import messages  # noqa: E402


def field(type_name: str, field_name: str) -> dict:
    definition = messages._SCHEMA["messages"].get(type_name)  # pylint: disable=protected-access
    assert definition is not None, f"missing message definition for {type_name}"
    for entry in definition["fields"]:
        if entry["name"] == field_name:
            return entry
    raise AssertionError(f"missing field {type_name}.{field_name}")


def has_field(type_name: str, field_name: str) -> bool:
    definition = messages._SCHEMA["messages"].get(type_name)  # pylint: disable=protected-access
    if definition is None:
        return False
    return any(entry["name"] == field_name for entry in definition["fields"])


def read_file(path: str) -> str:
    with open(path, "r", encoding="utf-8") as handle:
        return handle.read()


def verify_proto_stream_fields() -> None:
    assert field("ConnectTCPRequest", "stream_id")
    assert field("ListenTCPRequest", "stream_id")
    assert field("AcceptTCP", "listen_stream_id")
    assert field("AcceptTCP", "new_stream_id")
    assert field("StreamReply", "stream_id")
    assert field("StreamReply", "new_stream_id")

    assert not has_field("ConnectTCPRequest", "socket_number")
    assert not has_field("ListenTCPRequest", "socket_number")
    assert not has_field("AcceptTCP", "listen_socket_number")
    assert not has_field("AcceptTCP", "new_socket_number")
    assert not has_field("StreamReply", "socket_id")
    assert not has_field("StreamReply", "new_socket_id")


def verify_rsp_client_uses_stream_wire_fields() -> None:
    rsp_client_source = read_file(os.path.join(REPO_ROOT, "client", "python", "rsp_client.py"))

    required_tokens = (
        "_messages.STREAM_STATUS.SUCCESS",
        '"rsp.proto.StreamReply"',
        '"stream_id": {"value": stream_id}',
        '"listen_stream_id": {"value": listen_socket_id}',
        'tcp["new_stream_id"] = {"value": _normalize_guid(requested_stream_id)}',
        '_pack_service_message("StreamSend", {"stream_id": {"value": socket_id}, "data": data_b64})',
        '_pack_service_message("StreamRecv", {"stream_id": {"value": socket_id}, "max_bytes": max_bytes})',
        '_pack_service_message("StreamClose", {"stream_id": {"value": socket_id}})',
    )
    for token in required_tokens:
        assert token in rsp_client_source, f"expected token missing from rsp_client.py: {token}"

    forbidden_tokens = (
        "_messages.SOCKET_STATUS",
        '"socket_number"',
        '"listen_socket_number"',
        '"new_socket_number"',
        '_pack_service_message("SocketSend"',
        '_pack_service_message("SocketRecv"',
        '_pack_service_message("SocketClose"',
    )
    for token in forbidden_tokens:
        assert token not in rsp_client_source, f"legacy socket token still present in rsp_client.py: {token}"


def verify_rsp_net_uses_stream_reply_fields() -> None:
    rsp_net_source = read_file(os.path.join(REPO_ROOT, "client", "python", "rsp_net.py"))

    required_tokens = (
        "STREAM_DATA",
        "STREAM_CLOSED",
        "STREAM_ERROR",
        "stream_id = _stream_id_value(reply)",
        "new_stream_id = _new_stream_id_value(reply)",
        "data = base64.b64decode(data_b64) if data_b64 else b\"\"",
    )
    for token in required_tokens:
        assert token in rsp_net_source, f"expected token missing from rsp_net.py: {token}"

    forbidden_tokens = (
        "bytes.fromhex(data_hex)",
        '(reply or {}).get("socket_id", {}).get("value")',
    )
    for token in forbidden_tokens:
        assert token not in rsp_net_source, f"legacy socket token still present in rsp_net.py: {token}"


def main() -> None:
    verify_proto_stream_fields()
    verify_rsp_client_uses_stream_wire_fields()
    verify_rsp_net_uses_stream_reply_fields()
    print("python_stream_client_contract_test passed")


if __name__ == "__main__":
    main()
