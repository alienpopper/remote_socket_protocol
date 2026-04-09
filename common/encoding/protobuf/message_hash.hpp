#pragma once

// Canonical field-based SHA256 hashing for RSPMessage.
//
// The hash is computed by iterating each field of the message (and its
// sub-messages) in proto field-number order and feeding a canonical binary
// encoding of each value into SHA256.  The signature field (99) is
// deliberately excluded so the hash can serve as the data-to-sign.
//
// Canonical encoding rules per field type:
//   uint32 / int32 / enum  : 4 bytes, big-endian
//   uint64                 : 8 bytes, big-endian
//   bool                   : 1 byte  (0 or 1)
//   bytes / string         : 4-byte big-endian length, then raw bytes
//   message field          : recurse (only when has_xxx() is true)
//   optional scalar        : only included when has_xxx() is true
//   repeated field         : 4-byte count, then each element in order
//   oneof field            : the active field-number fed as uint32, then value
//
// Every included field is preceded by its proto field number encoded as a
// uint32 big-endian "tag", making the encoding unambiguous regardless of which
// optional fields happen to be present.
//
// Because the rules above are wire-format-agnostic, any other encoding (JSON,
// custom binary, …) that iterates fields in the same order produces an
// identical hash, enabling cross-encoding signature verification.

#include <array>
#include <cstdint>

#include "messages.pb.h"

namespace rsp::encoding::protobuf {

// A 32-byte SHA256 digest.
using MessageHash = std::array<uint8_t, 32>;

// Compute the canonical SHA256 hash of all RSPMessage fields except the
// outer signature (field 99).  Returns a 32-byte digest.
MessageHash computeMessageHash(const rsp::proto::RSPMessage& message);

}  // namespace rsp::encoding::protobuf
