#pragma once

#include "messages.pb.h"

#include <stdexcept>
#include <string>

namespace rsp::erd_text {

// Serialize a proto ERDAbstractSyntaxTree to human-readable text.
// Returns an empty string for an unset (NODE_TYPE_NOT_SET) tree.
//
// Text format:
//   Binary operators (each takes exactly two sub-trees):
//     AND(<tree>, <tree>)
//     OR(<tree>, <tree>)
//     EQ(<tree>, <tree>)
//   Leaf predicates:
//     TYPE(<uuid>)       -- matches endorsement type
//     VALUE(<hex>)       -- matches endorsement value bytes
//     SIGNER(<uuid>)     -- matches endorsement service node id
//
// UUIDs use standard 8-4-4-4-12 lowercase hex format.
// Byte arrays use lowercase hex pairs with no separators.
// Whitespace between tokens is ignored during parsing.
std::string toString(const rsp::proto::ERDAbstractSyntaxTree& tree);

// Parse a human-readable text string into a proto ERDAbstractSyntaxTree.
// Returns an unset tree for an empty or all-whitespace string.
// Throws std::runtime_error if the text is malformed.
rsp::proto::ERDAbstractSyntaxTree fromString(const std::string& text);

}  // namespace rsp::erd_text
