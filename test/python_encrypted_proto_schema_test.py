#!/usr/bin/env python3

import base64
import copy
import os
import sys


REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
if REPO_ROOT not in sys.path:
    sys.path.insert(0, REPO_ROOT)

from client.python import messages  # noqa: E402


def b64(value: bytes) -> str:
    return base64.b64encode(value).decode("ascii")


def field(type_name: str, field_name: str) -> dict:
    definition = messages._SCHEMA["messages"].get(type_name)  # pylint: disable=protected-access
    assert definition is not None, f"missing message definition for {type_name}"
    for entry in definition["fields"]:
        if entry["name"] == field_name:
            return entry
    raise AssertionError(f"missing field {type_name}.{field_name}")


def main() -> None:
    assert field("FieldEncryptionFixture", "clear_text").get("encrypted") is False
    assert field("FieldEncryptionFixture", "secret_text").get("encrypted") is True
    assert field("FieldEncryptionFixture", "secret_bytes").get("encrypted") is True
    assert field("RSPMessage", "aes_key_negotiation_request")
    assert field("RSPMessage", "aes_key_negotiation_reply")

    message = {
        "source": {"value": b64(b"source-node-12345")},
        "nonce": {"value": b64(b"message-nonce-123")},
        "ping_request": {
            "nonce": {"value": b64(b"ping-nonce-123456")},
            "sequence": 7,
        },
        "encrypted_fields": [
            {
                "path": {"segments": ["service_message", "secret_text"]},
                "iv": b64(b"123456789012"),
                "ciphertext": b64(b"ciphertext-a"),
                "tag": b64(b"0123456789ABCDEF"),
                "algorithm": 1,
            }
        ],
    }

    hash_a = messages.hash_rsp_message(message)
    modified = copy.deepcopy(message)
    modified["encrypted_fields"][0]["ciphertext"] = b64(b"ciphertext-b")
    hash_b = messages.hash_rsp_message(modified)
    assert hash_a != hash_b, "encrypted field data should affect canonical hash"

    key_message = {
        "source": {"value": b64(b"source-node-12345")},
        "destination": {"value": b64(b"destination-node")},
        "aes_key_negotiation_request": {
            "key_id": {"value": b64(b"1234567890ABCDEF")},
            "ephemeral_public_key": {
                "algorithm": messages.SIGNATURE_ALGORITHMS.P256,
                "public_key": b64(b"ephemeral-public-key"),
            },
            "requested_lifetime_ms": 5000,
            "algorithm": (
                messages.KEY_NEGOTIATION_ALGORITHM
                .KEY_NEGOTIATION_ALGORITHM_P256_SHA256_AES256
            ),
        },
    }
    key_hash_a = messages.hash_rsp_message(key_message)
    key_modified = copy.deepcopy(key_message)
    key_modified["aes_key_negotiation_request"]["key_id"]["value"] = b64(b"ABCDE1234567890F")
    key_hash_b = messages.hash_rsp_message(key_modified)
    assert key_hash_a != key_hash_b, "AES key negotiation fields should affect canonical hash"

    print("python_encrypted_proto_schema_test passed")


if __name__ == "__main__":
    main()
