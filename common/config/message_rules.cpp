#include "common/config/message_rules.hpp"

#include "common/endorsement/endorsement_text.hpp"
#include "common/keypair.hpp"

#include <cctype>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>

namespace rsp::config {

namespace {

std::string trimmed(const std::string& s) {
    std::size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) ++start;
    std::size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) --end;
    return s.substr(start, end - start);
}

bool isCommentOrBlank(const std::string& trimmedLine) {
    if (trimmedLine.empty()) return true;
    if (trimmedLine[0] == '#') return true;
    if (trimmedLine.size() >= 2 && trimmedLine[0] == '/' && trimmedLine[1] == '/') return true;
    return false;
}

bool isIdentifier(const std::string& s) {
    if (s.empty()) return false;
    auto isFirst = [](char c) {
        return c == '_' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    };
    auto isRest = [](char c) {
        return c == '_' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
               (c >= '0' && c <= '9');
    };
    if (!isFirst(s[0])) return false;
    for (std::size_t i = 1; i < s.size(); ++i) {
        if (!isRest(s[i])) return false;
    }
    return true;
}

// Try to match "let <name> = <expr>". Returns true and sets outName/outExpr if
// the trimmed line begins with "let " and contains "=". The caller still has
// to validate that the name is a valid identifier (we do that here too).
bool tryParseLet(const std::string& trimmedLine,
                 std::string& outName,
                 std::string& outExpr,
                 std::string& outError) {
    static const std::string kPrefix = "let ";
    if (trimmedLine.size() < kPrefix.size()) return false;
    bool startsWithLet = trimmedLine.compare(0, kPrefix.size(), kPrefix) == 0;
    // Also accept "let\t..." with any whitespace separator.
    if (!startsWithLet) {
        if (trimmedLine.compare(0, 3, "let") == 0 && trimmedLine.size() > 3 &&
            std::isspace(static_cast<unsigned char>(trimmedLine[3]))) {
            startsWithLet = true;
        }
    }
    if (!startsWithLet) return false;

    std::size_t i = 3;
    while (i < trimmedLine.size() && std::isspace(static_cast<unsigned char>(trimmedLine[i]))) ++i;
    const std::size_t nameStart = i;
    while (i < trimmedLine.size() && !std::isspace(static_cast<unsigned char>(trimmedLine[i])) &&
           trimmedLine[i] != '=') {
        ++i;
    }
    const std::string name = trimmedLine.substr(nameStart, i - nameStart);
    while (i < trimmedLine.size() && std::isspace(static_cast<unsigned char>(trimmedLine[i]))) ++i;
    if (i >= trimmedLine.size() || trimmedLine[i] != '=') {
        outError = "let declaration is missing '='";
        outName = name;
        return true;
    }
    ++i;  // consume '='
    while (i < trimmedLine.size() && std::isspace(static_cast<unsigned char>(trimmedLine[i]))) ++i;

    if (!isIdentifier(name)) {
        outError = "invalid variable name '" + name + "'";
        outName = name;
        return true;
    }

    outName = name;
    outExpr = trimmedLine.substr(i);
    if (outExpr.empty()) {
        outError = "let declaration for '" + name + "' has empty expression";
        return true;
    }
    outError.clear();
    return true;
}

}  // namespace

std::string expandMacros(const std::string& input, const MacroContext& ctx) {
    std::string result = input;

    const std::string myNodeId = "MY_NODEID()";
    std::size_t pos = 0;
    while ((pos = result.find(myNodeId, pos)) != std::string::npos) {
        result.replace(pos, myNodeId.size(), ctx.nodeId);
        pos += ctx.nodeId.size();
    }

    const std::string fromFilePrefix = "NODEID_FROM_FILE(";
    pos = 0;
    while ((pos = result.find(fromFilePrefix, pos)) != std::string::npos) {
        std::size_t argStart = pos + fromFilePrefix.size();
        std::size_t argEnd = result.find(')', argStart);
        if (argEnd == std::string::npos) {
            throw std::runtime_error("NODEID_FROM_FILE( missing closing ')'");
        }
        std::string path = result.substr(argStart, argEnd - argStart);
        rsp::NodeID id = rsp::KeyPair::nodeIDFromPublicKeyFile(path);
        std::string replacement = id.toString();
        result.replace(pos, (argEnd + 1) - pos, replacement);
        pos += replacement.size();
    }

    return result;
}

rsp::proto::ERDAbstractSyntaxTree buildMessageRulesTree(
    const nlohmann::json& rules,
    const MacroContext& macros) {

    if (!rules.is_array()) {
        throw std::runtime_error("\"message_rules\" must be a JSON array");
    }

    std::map<std::string, rsp::proto::ERDAbstractSyntaxTree> variables;

    rsp::erd_text::VariableResolver resolver =
        [&variables](const std::string& name)
            -> std::optional<rsp::proto::ERDAbstractSyntaxTree> {
        auto it = variables.find(name);
        if (it == variables.end()) {
            return std::nullopt;
        }
        return it->second;  // returned by value (copy)
    };

    bool ruleSeen = false;
    rsp::proto::ERDAbstractSyntaxTree ruleTree;

    for (std::size_t idx = 0; idx < rules.size(); ++idx) {
        const auto& entry = rules[idx];
        if (!entry.is_string()) {
            throw std::runtime_error(
                "message_rules[" + std::to_string(idx + 1) + "]: entry must be a string");
        }
        const std::string raw = entry.get<std::string>();
        const std::string line = trimmed(raw);

        if (isCommentOrBlank(line)) {
            continue;
        }

        if (ruleSeen) {
            throw std::runtime_error(
                "message_rules[" + std::to_string(idx + 1) +
                "]: content appears after the rule line; the rule line must be last");
        }

        std::string letName;
        std::string letExpr;
        std::string letError;
        if (tryParseLet(line, letName, letExpr, letError)) {
            if (!letError.empty()) {
                throw std::runtime_error(
                    "message_rules[" + std::to_string(idx + 1) + "]: " + letError);
            }
            if (variables.count(letName) != 0) {
                throw std::runtime_error(
                    "message_rules[" + std::to_string(idx + 1) +
                    "]: duplicate variable '" + letName + "'");
            }
            std::string expanded;
            try {
                expanded = expandMacros(letExpr, macros);
            } catch (const std::exception& e) {
                throw std::runtime_error(
                    "message_rules[" + std::to_string(idx + 1) +
                    "]: macro expansion failed: " + e.what());
            }
            rsp::proto::ERDAbstractSyntaxTree tree;
            try {
                tree = rsp::erd_text::fromString(expanded, resolver);
            } catch (const std::exception& e) {
                throw std::runtime_error(
                    "message_rules[" + std::to_string(idx + 1) +
                    "]: failed to parse let '" + letName + "': " + e.what());
            }
            variables.emplace(letName, std::move(tree));
            continue;
        }

        // Rule line.
        std::string expanded;
        try {
            expanded = expandMacros(line, macros);
        } catch (const std::exception& e) {
            throw std::runtime_error(
                "message_rules[" + std::to_string(idx + 1) +
                "]: macro expansion failed: " + e.what());
        }
        try {
            ruleTree = rsp::erd_text::fromString(expanded, resolver);
        } catch (const std::exception& e) {
            throw std::runtime_error(
                "message_rules[" + std::to_string(idx + 1) +
                "]: failed to parse rule: " + e.what());
        }
        ruleSeen = true;
    }

    if (!ruleSeen) {
        throw std::runtime_error("\"message_rules\" must contain exactly one rule line");
    }

    return ruleTree;
}

}  // namespace rsp::config
