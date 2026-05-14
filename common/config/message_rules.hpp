#pragma once

#include "messages.pb.h"
#include "third_party/json/single_include/nlohmann/json.hpp"

#include <string>

namespace rsp::config {

// Variables substituted into rule strings before ERD parsing.
// MY_NODEID() expands to MacroContext::nodeId.
struct MacroContext {
    std::string nodeId;  // hex string from KeyPair::nodeID().toString()
};

// Apply macro expansion to a rule string. Recognized macros:
//   MY_NODEID()              -> MacroContext::nodeId
//   NODEID_FROM_FILE(<path>) -> NodeID derived from a public-key PEM file
// Throws std::runtime_error on malformed macro syntax or unreadable file.
std::string expandMacros(const std::string& input, const MacroContext& ctx);

// Build the authorization tree from a JSON array of rule strings.
//
// Each entry is a "line" of a tiny config DSL:
//   - blank lines and lines whose trimmed content begins with "#" or "//" are
//     skipped;
//   - "let <name> = <expr>" declares a variable; <expr> is macro-expanded and
//     then parsed by rsp::erd_text::fromString with a resolver that knows the
//     previously declared variables. The resulting AST is stored under <name>;
//   - exactly one non-let, non-comment, non-blank line ("rule line") is
//     required, and it must be the LAST such line. Its parsed AST is returned.
//
// The returned tree is the rule line's AST. Variables referenced via ${name}
// are spliced in by the parser as deep copies.
//
// On any error a std::runtime_error is thrown whose message identifies the
// 1-based JSON-array index of the offending entry.
rsp::proto::ERDAbstractSyntaxTree buildMessageRulesTree(
    const nlohmann::json& rules,
    const MacroContext& macros);

}  // namespace rsp::config
