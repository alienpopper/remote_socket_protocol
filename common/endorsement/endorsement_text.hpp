#pragma once

#include "messages.pb.h"

#include <functional>
#include <optional>
#include <stdexcept>
#include <string>

namespace rsp::erd_text {

// Resolver callback consulted by the parser whenever a ${name} atom is
// encountered. Return an unset optional to signal that the name is undeclared
// (the parser will throw with a message identifying the offending variable).
using VariableResolver = std::function<
    std::optional<rsp::proto::ERDAbstractSyntaxTree>(const std::string& name)>;

// Serialize a proto ERDAbstractSyntaxTree to human-readable text.
// Returns an empty string for an unset (NODE_TYPE_NOT_SET) tree.
//
// Text format:
//   Binary operators (each takes exactly two sub-trees):
//     AND(<tree>, <tree>)
//     OR(<tree>, <tree>)
//     EQ(<tree>, <tree>)
//   N-ary operators:
//     ALLOF(<tree>, <tree>, ...)
//     ANYOF(<tree>, <tree>, ...)
//   Constant nodes:
//     TRUE
//     FALSE
//   Leaf predicates:
//     ENDORSEMENT_TYPE(<uuid>)    -- matches endorsement type
//     ENDORSEMENT_VALUE(<hex>)    -- matches endorsement value bytes
//     ENDORSEMENT_SIGNER(<uuid>)  -- matches endorsement service node id
//
// UUIDs use standard 8-4-4-4-12 lowercase hex format.
// Byte arrays use lowercase hex pairs with no separators.
// Whitespace between tokens is ignored during parsing.
std::string toString(const rsp::proto::ERDAbstractSyntaxTree& tree);

// Parse a human-readable text string into a proto ERDAbstractSyntaxTree.
// Returns an unset tree for an empty or all-whitespace string.
// Throws std::runtime_error if the text is malformed.
rsp::proto::ERDAbstractSyntaxTree fromString(const std::string& text);

// Same as fromString(text) but additionally recognizes ${name} as an atom.
// When ${name} appears, the parser invokes 'resolver(name)' and splices the
// returned subtree into the parse output (deep copy). If 'resolver' returns
// nullopt the parser throws a "${name}: undeclared variable" error.
rsp::proto::ERDAbstractSyntaxTree fromString(const std::string& text,
                                             const VariableResolver& resolver);

}  // namespace rsp::erd_text
