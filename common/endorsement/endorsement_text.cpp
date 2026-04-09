#include "common/endorsement/endorsement_text.hpp"

#include <cctype>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

// ---- Serialization helpers ----

const char kHexChars[] = "0123456789abcdef";

bool isHexChar(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

// Convert 16 raw bytes (big-endian high/low uint64 layout) to UUID string.
std::string bytesToUUID(const std::string& bytes) {
    if (bytes.size() != 16) {
        throw std::runtime_error("UUID field must be exactly 16 bytes");
    }
    // Layout: bytes[0..7] = high uint64, bytes[8..15] = low uint64, big-endian.
    // UUID string groups: 8-4-4-4-12 hex chars.
    static const int kGroupSizes[] = {4, 2, 2, 2, 6};  // bytes per group
    static const char kSep[] = {0, '-', '-', '-', '-'};
    std::string result;
    result.reserve(36);
    std::size_t bytePos = 0;
    for (int g = 0; g < 5; ++g) {
        if (kSep[g]) result += kSep[g];
        for (int b = 0; b < kGroupSizes[g]; ++b) {
            const uint8_t byte = static_cast<uint8_t>(bytes[bytePos++]);
            result += kHexChars[byte >> 4];
            result += kHexChars[byte & 0x0F];
        }
    }
    return result;
}

// Convert 16-byte UUID string (8-4-4-4-12 format) to raw big-endian bytes.
std::string uuidToBytes(const std::string& uuid) {
    // Remove dashes and decode 32 hex chars.
    std::string hex;
    hex.reserve(32);
    for (char c : uuid) {
        if (c != '-') hex += c;
    }
    if (hex.size() != 32) {
        throw std::runtime_error("invalid UUID: expected 32 hex chars");
    }
    auto hexVal = [](char c) -> uint8_t {
        if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
        if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
        if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(c - 'A' + 10);
        throw std::runtime_error(std::string("invalid hex char in UUID: ") + c);
    };
    std::string result;
    result.reserve(16);
    for (std::size_t i = 0; i < 32; i += 2) {
        result += static_cast<char>((hexVal(hex[i]) << 4) | hexVal(hex[i + 1]));
    }
    return result;
}

std::string bytesToHex(const std::string& bytes) {
    std::string result;
    result.reserve(bytes.size() * 2);
    for (unsigned char c : bytes) {
        result += kHexChars[c >> 4];
        result += kHexChars[c & 0x0F];
    }
    return result;
}

void appendTree(const rsp::proto::ERDAbstractSyntaxTree& tree, std::string& out);

void appendBinary(const char* op,
                  const rsp::proto::ERDAbstractSyntaxTree& lhs,
                  const rsp::proto::ERDAbstractSyntaxTree& rhs,
                  std::string& out) {
    out += op;
    out += '(';
    appendTree(lhs, out);
    out += ", ";
    appendTree(rhs, out);
    out += ')';
}

void appendNary(const char* op,
                const google::protobuf::RepeatedPtrField<rsp::proto::ERDAbstractSyntaxTree>& terms,
                std::string& out) {
    out += op;
    out += '(';
    for (int index = 0; index < terms.size(); ++index) {
        if (index != 0) {
            out += ", ";
        }
        appendTree(terms.Get(index), out);
    }
    out += ')';
}

void appendTree(const rsp::proto::ERDAbstractSyntaxTree& tree, std::string& out) {
    switch (tree.node_type_case()) {
        case rsp::proto::ERDAbstractSyntaxTree::kAnd:
            appendBinary("AND", tree.and_().lhs(), tree.and_().rhs(), out);
            break;
        case rsp::proto::ERDAbstractSyntaxTree::kOr:
            appendBinary("OR", tree.or_().lhs(), tree.or_().rhs(), out);
            break;
        case rsp::proto::ERDAbstractSyntaxTree::kEquals:
            appendBinary("EQ", tree.equals().lhs(), tree.equals().rhs(), out);
            break;
        case rsp::proto::ERDAbstractSyntaxTree::kAllOf:
            appendNary("ALLOF", tree.all_of().terms(), out);
            break;
        case rsp::proto::ERDAbstractSyntaxTree::kAnyOf:
            appendNary("ANYOF", tree.any_of().terms(), out);
            break;
        case rsp::proto::ERDAbstractSyntaxTree::kTypeEquals:
            out += "TYPE(";
            out += bytesToUUID(tree.type_equals().type().value());
            out += ')';
            break;
        case rsp::proto::ERDAbstractSyntaxTree::kValueEquals:
            out += "VALUE(";
            out += bytesToHex(tree.value_equals().value());
            out += ')';
            break;
        case rsp::proto::ERDAbstractSyntaxTree::kSignerEquals:
            out += "SIGNER(";
            out += bytesToUUID(tree.signer_equals().signer().value());
            out += ')';
            break;
        case rsp::proto::ERDAbstractSyntaxTree::kTrueValue:
            out += "TRUE";
            break;
        case rsp::proto::ERDAbstractSyntaxTree::kFalseValue:
            out += "FALSE";
            break;
        case rsp::proto::ERDAbstractSyntaxTree::NODE_TYPE_NOT_SET:
            break;
    }
}

// ---- Recursive descent parser ----

struct Parser {
    explicit Parser(const std::string& t) : text(t), pos(0) {}

    const std::string& text;
    std::size_t pos;

    void skipWhitespace() {
        while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) {
            ++pos;
        }
    }

    char peek() const {
        return pos < text.size() ? text[pos] : '\0';
    }

    void expect(char c) {
        skipWhitespace();
        if (peek() != c) {
            throw std::runtime_error(
                std::string("expected '") + c + "' in ERD text at position " + std::to_string(pos));
        }
        ++pos;
    }

    bool startsWith(const char* prefix, std::size_t len) const {
        return pos + len <= text.size() && text.compare(pos, len, prefix, len) == 0;
    }

    uint8_t hexValue(char c) const {
        if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
        if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
        if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(c - 'A' + 10);
        throw std::runtime_error(
            std::string("invalid hex character '") + c + "' at position " + std::to_string(pos));
    }

    // Parse UUID (8-4-4-4-12 format) and return raw 16-byte big-endian string.
    bool isUUID() const {
        static const int kGroups[] = {8, 4, 4, 4, 12};
        std::size_t p = pos;
        for (int g = 0; g < 5; ++g) {
            if (g > 0) {
                if (p >= text.size() || text[p++] != '-') return false;
            }
            for (int i = 0; i < kGroups[g]; ++i) {
                if (p >= text.size() || !isHexChar(text[p++])) return false;
            }
        }
        return true;
    }

    std::string parseUUIDBytes() {
        if (!isUUID()) {
            throw std::runtime_error("expected UUID at position " + std::to_string(pos));
        }
        std::string uuid = text.substr(pos, 36);
        pos += 36;
        return uuidToBytes(uuid);
    }

    std::string parseHexBytes() {
        std::string result;
        while (pos < text.size() && isHexChar(text[pos])) {
            if (pos + 1 >= text.size() || !isHexChar(text[pos + 1])) {
                throw std::runtime_error(
                    "odd number of hex characters at position " + std::to_string(pos));
            }
            const uint8_t hi = hexValue(text[pos++]);
            const uint8_t lo = hexValue(text[pos++]);
            result += static_cast<char>((hi << 4) | lo);
        }
        return result;
    }

    // Parse a binary operator: reads "(lhs, rhs)"
    void parseBinaryArgs(rsp::proto::ERDAbstractSyntaxTree* lhs,
                         rsp::proto::ERDAbstractSyntaxTree* rhs) {
        expect('(');
        *lhs = parseTree();
        skipWhitespace();
        expect(',');
        *rhs = parseTree();
        skipWhitespace();
        expect(')');
    }

    void parseNaryArgs(google::protobuf::RepeatedPtrField<rsp::proto::ERDAbstractSyntaxTree>* terms) {
        expect('(');
        skipWhitespace();
        if (peek() == ')') {
            ++pos;
            return;
        }

        while (true) {
            *terms->Add() = parseTree();
            skipWhitespace();
            if (peek() == ',') {
                ++pos;
                continue;
            }

            expect(')');
            return;
        }
    }

    rsp::proto::ERDAbstractSyntaxTree parseTree() {
        skipWhitespace();
        rsp::proto::ERDAbstractSyntaxTree tree;

        if (startsWith("AND(", 4)) {
            pos += 3;  // skip "AND", leave '(' for parseBinaryArgs
            parseBinaryArgs(tree.mutable_and_()->mutable_lhs(),
                            tree.mutable_and_()->mutable_rhs());
            return tree;
        }
        if (startsWith("OR(", 3)) {
            pos += 2;
            parseBinaryArgs(tree.mutable_or_()->mutable_lhs(),
                            tree.mutable_or_()->mutable_rhs());
            return tree;
        }
        if (startsWith("EQ(", 3)) {
            pos += 2;
            parseBinaryArgs(tree.mutable_equals()->mutable_lhs(),
                            tree.mutable_equals()->mutable_rhs());
            return tree;
        }
        if (startsWith("ALLOF(", 6)) {
            pos += 5;
            parseNaryArgs(tree.mutable_all_of()->mutable_terms());
            return tree;
        }
        if (startsWith("ANYOF(", 6)) {
            pos += 5;
            parseNaryArgs(tree.mutable_any_of()->mutable_terms());
            return tree;
        }
        if (startsWith("TYPE(", 5)) {
            pos += 5;
            skipWhitespace();
            tree.mutable_type_equals()->mutable_type()->set_value(parseUUIDBytes());
            skipWhitespace();
            expect(')');
            return tree;
        }
        if (startsWith("VALUE(", 6)) {
            pos += 6;
            skipWhitespace();
            tree.mutable_value_equals()->set_value(parseHexBytes());
            skipWhitespace();
            expect(')');
            return tree;
        }
        if (startsWith("SIGNER(", 7)) {
            pos += 7;
            skipWhitespace();
            tree.mutable_signer_equals()->mutable_signer()->set_value(parseUUIDBytes());
            skipWhitespace();
            expect(')');
            return tree;
        }
        if (startsWith("TRUE", 4)) {
            pos += 4;
            tree.mutable_true_value();
            return tree;
        }
        if (startsWith("FALSE", 5)) {
            pos += 5;
            tree.mutable_false_value();
            return tree;
        }

        throw std::runtime_error(
            "unexpected token in ERD text at position " + std::to_string(pos));
    }
};

}  // namespace

namespace rsp::erd_text {

std::string toString(const rsp::proto::ERDAbstractSyntaxTree& tree) {
    if (tree.node_type_case() == rsp::proto::ERDAbstractSyntaxTree::NODE_TYPE_NOT_SET) {
        return {};
    }
    std::string result;
    appendTree(tree, result);
    return result;
}

rsp::proto::ERDAbstractSyntaxTree fromString(const std::string& text) {
    Parser parser(text);
    parser.skipWhitespace();
    if (parser.pos >= text.size()) {
        return rsp::proto::ERDAbstractSyntaxTree();
    }
    rsp::proto::ERDAbstractSyntaxTree tree = parser.parseTree();
    parser.skipWhitespace();
    if (parser.pos < text.size()) {
        throw std::runtime_error(
            "unexpected content after ERD expression at position " + std::to_string(parser.pos));
    }
    return tree;
}

}  // namespace rsp::erd_text
