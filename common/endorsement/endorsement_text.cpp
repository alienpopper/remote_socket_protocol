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

void appendEndTree(const rsp::proto::ERDASTEndTree& tree, std::string& out);
void appendMessageTree(const rsp::proto::ERDASTMessageTree& tree, std::string& out);

void appendEndBinary(const char* op,
                     const rsp::proto::ERDASTEndTree& lhs,
                     const rsp::proto::ERDASTEndTree& rhs,
                     std::string& out) {
    out += op;
    out += '(';
    appendEndTree(lhs, out);
    out += ", ";
    appendEndTree(rhs, out);
    out += ')';
}

void appendEndNary(const char* op,
                   const google::protobuf::RepeatedPtrField<rsp::proto::ERDASTEndTree>& terms,
                   std::string& out) {
    out += op;
    out += '(';
    for (int index = 0; index < terms.size(); ++index) {
        if (index != 0) {
            out += ", ";
        }
        appendEndTree(terms.Get(index), out);
    }
    out += ')';
}

void appendEndTree(const rsp::proto::ERDASTEndTree& tree, std::string& out) {
    switch (tree.node_type_case()) {
        case rsp::proto::ERDASTEndTree::kAnd:
            appendEndBinary("AND", tree.and_().lhs(), tree.and_().rhs(), out);
            break;
        case rsp::proto::ERDASTEndTree::kOr:
            appendEndBinary("OR", tree.or_().lhs(), tree.or_().rhs(), out);
            break;
        case rsp::proto::ERDASTEndTree::kEquals:
            appendEndBinary("EQ", tree.equals().lhs(), tree.equals().rhs(), out);
            break;
        case rsp::proto::ERDASTEndTree::kAllOf:
            appendEndNary("ALLOF", tree.all_of().terms(), out);
            break;
        case rsp::proto::ERDASTEndTree::kAnyOf:
            appendEndNary("ANYOF", tree.any_of().terms(), out);
            break;
        case rsp::proto::ERDASTEndTree::kTypeEquals:
            out += "ENDORSEMENT_TYPE(";
            out += bytesToUUID(tree.type_equals().type().value());
            out += ')';
            break;
        case rsp::proto::ERDASTEndTree::kValueEquals:
            out += "ENDORSEMENT_VALUE(";
            out += bytesToHex(tree.value_equals().value());
            out += ')';
            break;
        case rsp::proto::ERDASTEndTree::kSignerEquals:
            out += "ENDORSEMENT_SIGNER(";
            out += bytesToUUID(tree.signer_equals().signer().value());
            out += ')';
            break;
        case rsp::proto::ERDASTEndTree::kTrueValue:
            out += "TRUE";
            break;
        case rsp::proto::ERDASTEndTree::kFalseValue:
            out += "FALSE";
            break;
        case rsp::proto::ERDASTEndTree::NODE_TYPE_NOT_SET:
            break;
    }
}

void appendMesBinary(const char* op,
                     const rsp::proto::ERDASTMessageTree& lhs,
                     const rsp::proto::ERDASTMessageTree& rhs,
                     std::string& out) {
    out += op;
    out += '(';
    appendMessageTree(lhs, out);
    out += ", ";
    appendMessageTree(rhs, out);
    out += ')';
}

void appendMesNary(const char* op,
                   const google::protobuf::RepeatedPtrField<rsp::proto::ERDASTMessageTree>& terms,
                   std::string& out) {
    out += op;
    out += '(';
    for (int index = 0; index < terms.size(); ++index) {
        if (index != 0) {
            out += ", ";
        }
        appendMessageTree(terms.Get(index), out);
    }
    out += ')';
}

void appendFieldPath(const rsp::proto::ERDASTFieldPath& path, std::string& out) {
    for (int i = 0; i < path.segments_size(); ++i) {
        if (i > 0) out += '.';
        out += path.segments(i);
    }
}

void appendFieldValue(const rsp::proto::ERDASTFieldValue& val, std::string& out) {
    switch (val.value_case()) {
        case rsp::proto::ERDASTFieldValue::kBytesValue:
            out += "0x";
            out += bytesToHex(val.bytes_value());
            break;
        case rsp::proto::ERDASTFieldValue::kStringValue:
            out += '"';
            out += val.string_value();
            out += '"';
            break;
        case rsp::proto::ERDASTFieldValue::kIntValue:
            out += std::to_string(val.int_value());
            break;
        case rsp::proto::ERDASTFieldValue::kUintValue:
            out += std::to_string(val.uint_value());
            out += 'u';
            break;
        case rsp::proto::ERDASTFieldValue::kBoolValue:
            out += val.bool_value() ? "true" : "false";
            break;
        case rsp::proto::ERDASTFieldValue::kEnumValue:
            out += "enum(";
            out += std::to_string(val.enum_value());
            out += ')';
            break;
        case rsp::proto::ERDASTFieldValue::VALUE_NOT_SET:
            break;
    }
}

void appendMessageTree(const rsp::proto::ERDASTMessageTree& tree, std::string& out) {
    switch (tree.node_type_case()) {
        case rsp::proto::ERDASTMessageTree::kAnd:
            appendMesBinary("AND", tree.and_().lhs(), tree.and_().rhs(), out);
            break;
        case rsp::proto::ERDASTMessageTree::kOr:
            appendMesBinary("OR", tree.or_().lhs(), tree.or_().rhs(), out);
            break;
        case rsp::proto::ERDASTMessageTree::kEquals:
            appendMesBinary("EQ", tree.equals().lhs(), tree.equals().rhs(), out);
            break;
        case rsp::proto::ERDASTMessageTree::kAllOf:
            appendMesNary("ALLOF", tree.all_of().terms(), out);
            break;
        case rsp::proto::ERDASTMessageTree::kAnyOf:
            appendMesNary("ANYOF", tree.any_of().terms(), out);
            break;
        case rsp::proto::ERDASTMessageTree::kFieldEquals:
            out += "FIELD_EQ(";
            appendFieldPath(tree.field_equals().path(), out);
            out += ", ";
            appendFieldValue(tree.field_equals().value(), out);
            out += ')';
            break;
        case rsp::proto::ERDASTMessageTree::kFieldExists:
            out += "FIELD_EXISTS(";
            appendFieldPath(tree.field_exists().path(), out);
            out += ')';
            break;
        case rsp::proto::ERDASTMessageTree::kFieldContains:
            out += "FIELD_CONTAINS(";
            appendFieldPath(tree.field_contains().path(), out);
            out += ", ";
            appendFieldPath(tree.field_contains().sub_path(), out);
            out += ", ";
            appendFieldValue(tree.field_contains().value(), out);
            out += ')';
            break;
        case rsp::proto::ERDASTMessageTree::kTrueValue:
            out += "TRUE";
            break;
        case rsp::proto::ERDASTMessageTree::kFalseValue:
            out += "FALSE";
            break;
        case rsp::proto::ERDASTMessageTree::NODE_TYPE_NOT_SET:
            break;
    }
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
        case rsp::proto::ERDAbstractSyntaxTree::kEndorsement:
            out += "ENDORSEMENT(";
            appendEndTree(tree.endorsement().tree(), out);
            out += ')';
            break;
        case rsp::proto::ERDAbstractSyntaxTree::kMessage:
            out += "MESSAGE(";
            appendMessageTree(tree.message().tree(), out);
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

    bool startsWithCI(const char* prefix, std::size_t len) const {
        if (pos + len > text.size()) return false;
        for (std::size_t i = 0; i < len; ++i) {
            if (std::toupper(static_cast<unsigned char>(text[pos + i])) !=
                std::toupper(static_cast<unsigned char>(prefix[i])))
                return false;
        }
        return true;
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

    // Endorsement sub-tree parsing
    void parseEndBinaryArgs(rsp::proto::ERDASTEndTree* lhs,
                            rsp::proto::ERDASTEndTree* rhs) {
        expect('(');
        *lhs = parseEndTree();
        skipWhitespace();
        expect(',');
        *rhs = parseEndTree();
        skipWhitespace();
        expect(')');
    }

    void parseEndNaryArgs(google::protobuf::RepeatedPtrField<rsp::proto::ERDASTEndTree>* terms) {
        expect('(');
        skipWhitespace();
        if (peek() == ')') {
            ++pos;
            return;
        }

        while (true) {
            *terms->Add() = parseEndTree();
            skipWhitespace();
            if (peek() == ',') {
                ++pos;
                continue;
            }

            expect(')');
            return;
        }
    }

    rsp::proto::ERDASTEndTree parseEndTree() {
        skipWhitespace();
        rsp::proto::ERDASTEndTree tree;

        if (startsWith("AND(", 4)) {
            pos += 3;
            parseEndBinaryArgs(tree.mutable_and_()->mutable_lhs(),
                               tree.mutable_and_()->mutable_rhs());
            return tree;
        }
        if (startsWith("OR(", 3)) {
            pos += 2;
            parseEndBinaryArgs(tree.mutable_or_()->mutable_lhs(),
                               tree.mutable_or_()->mutable_rhs());
            return tree;
        }
        if (startsWith("EQ(", 3)) {
            pos += 2;
            parseEndBinaryArgs(tree.mutable_equals()->mutable_lhs(),
                               tree.mutable_equals()->mutable_rhs());
            return tree;
        }
        if (startsWith("ALLOF(", 6)) {
            pos += 5;
            parseEndNaryArgs(tree.mutable_all_of()->mutable_terms());
            return tree;
        }
        if (startsWith("ANYOF(", 6)) {
            pos += 5;
            parseEndNaryArgs(tree.mutable_any_of()->mutable_terms());
            return tree;
        }
        if (startsWith("ENDORSEMENT_TYPE(", 17)) {
            pos += 17;
            skipWhitespace();
            tree.mutable_type_equals()->mutable_type()->set_value(parseUUIDBytes());
            skipWhitespace();
            expect(')');
            return tree;
        }
        if (startsWith("ENDORSEMENT_VALUE(", 18)) {
            pos += 18;
            skipWhitespace();
            tree.mutable_value_equals()->set_value(parseHexBytes());
            skipWhitespace();
            expect(')');
            return tree;
        }
        if (startsWith("ENDORSEMENT_SIGNER(", 19)) {
            pos += 19;
            skipWhitespace();
            tree.mutable_signer_equals()->mutable_signer()->set_value(parseUUIDBytes());
            skipWhitespace();
            expect(')');
            return tree;
        }
        if (startsWithCI("TRUE", 4)) {
            pos += 4;
            tree.mutable_true_value();
            return tree;
        }
        if (startsWithCI("FALSE", 5)) {
            pos += 5;
            tree.mutable_false_value();
            return tree;
        }

        throw std::runtime_error(
            "unexpected token in endorsement sub-tree at position " + std::to_string(pos));
    }

    // Message sub-tree parsing
    void parseMesBinaryArgs(rsp::proto::ERDASTMessageTree* lhs,
                            rsp::proto::ERDASTMessageTree* rhs) {
        expect('(');
        *lhs = parseMessageTree();
        skipWhitespace();
        expect(',');
        *rhs = parseMessageTree();
        skipWhitespace();
        expect(')');
    }

    // Parse a dot-separated field path (e.g. "destination.value")
    // Stops at ',' or ')'.
    rsp::proto::ERDASTFieldPath parseFieldPath() {
        skipWhitespace();
        rsp::proto::ERDASTFieldPath path;
        std::string segment;
        while (pos < text.size()) {
            const char c = text[pos];
            if (c == '.' ) {
                if (segment.empty()) {
                    throw std::runtime_error("empty path segment at position " + std::to_string(pos));
                }
                path.add_segments(std::move(segment));
                segment.clear();
                ++pos;
                continue;
            }
            if (c == ',' || c == ')' || c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                break;
            }
            segment += c;
            ++pos;
        }
        if (!segment.empty()) {
            path.add_segments(std::move(segment));
        }
        if (path.segments_size() == 0) {
            throw std::runtime_error("empty field path at position " + std::to_string(pos));
        }
        return path;
    }

    // Parse a typed literal value.
    rsp::proto::ERDASTFieldValue parseFieldValue() {
        skipWhitespace();
        rsp::proto::ERDASTFieldValue val;
        if (pos >= text.size()) {
            throw std::runtime_error("expected field value at end of input");
        }

        // Quoted string
        if (text[pos] == '"') {
            ++pos;
            std::string s;
            while (pos < text.size() && text[pos] != '"') {
                s += text[pos++];
            }
            if (pos >= text.size()) {
                throw std::runtime_error("unterminated string literal");
            }
            ++pos;  // skip closing quote
            val.set_string_value(std::move(s));
            return val;
        }

        // Hex bytes: 0x...
        if (pos + 1 < text.size() && text[pos] == '0' && (text[pos + 1] == 'x' || text[pos + 1] == 'X')) {
            pos += 2;
            val.set_bytes_value(parseHexBytes());
            return val;
        }

        // Bool
        if (startsWithCI("true", 4) && (pos + 4 >= text.size() || !std::isalnum(text[pos + 4]))) {
            pos += 4;
            val.set_bool_value(true);
            return val;
        }
        if (startsWithCI("false", 5) && (pos + 5 >= text.size() || !std::isalnum(text[pos + 5]))) {
            pos += 5;
            val.set_bool_value(false);
            return val;
        }

        // Enum: enum(N)
        if (startsWith("enum(", 5)) {
            pos += 5;
            skipWhitespace();
            bool negative = false;
            if (pos < text.size() && text[pos] == '-') { negative = true; ++pos; }
            int64_t n = 0;
            while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
                n = n * 10 + (text[pos++] - '0');
            }
            if (negative) n = -n;
            skipWhitespace();
            expect(')');
            val.set_enum_value(static_cast<int32_t>(n));
            return val;
        }

        // Unsigned integer (trailing 'u')
        // Or signed integer (optional leading '-')
        {
            bool negative = false;
            std::size_t startPos = pos;
            if (pos < text.size() && text[pos] == '-') {
                negative = true;
                ++pos;
            }
            if (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
                uint64_t n = 0;
                while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
                    n = n * 10 + static_cast<uint64_t>(text[pos++] - '0');
                }
                if (!negative && pos < text.size() && (text[pos] == 'u' || text[pos] == 'U')) {
                    ++pos;
                    val.set_uint_value(n);
                    return val;
                }
                int64_t signed_n = static_cast<int64_t>(n);
                if (negative) signed_n = -signed_n;
                val.set_int_value(signed_n);
                return val;
            }
            pos = startPos;  // reset if not a number
        }

        throw std::runtime_error("unexpected field value at position " + std::to_string(pos));
    }

    void parseMesNaryArgs(google::protobuf::RepeatedPtrField<rsp::proto::ERDASTMessageTree>* terms) {
        expect('(');
        skipWhitespace();
        if (peek() == ')') {
            ++pos;
            return;
        }

        while (true) {
            *terms->Add() = parseMessageTree();
            skipWhitespace();
            if (peek() == ',') {
                ++pos;
                continue;
            }

            expect(')');
            return;
        }
    }

    rsp::proto::ERDASTMessageTree parseMessageTree() {
        skipWhitespace();
        rsp::proto::ERDASTMessageTree tree;

        if (startsWith("AND(", 4)) {
            pos += 3;
            parseMesBinaryArgs(tree.mutable_and_()->mutable_lhs(),
                               tree.mutable_and_()->mutable_rhs());
            return tree;
        }
        if (startsWith("OR(", 3)) {
            pos += 2;
            parseMesBinaryArgs(tree.mutable_or_()->mutable_lhs(),
                               tree.mutable_or_()->mutable_rhs());
            return tree;
        }
        if (startsWith("EQ(", 3)) {
            pos += 2;
            parseMesBinaryArgs(tree.mutable_equals()->mutable_lhs(),
                               tree.mutable_equals()->mutable_rhs());
            return tree;
        }
        if (startsWith("ALLOF(", 6)) {
            pos += 5;
            parseMesNaryArgs(tree.mutable_all_of()->mutable_terms());
            return tree;
        }
        if (startsWith("ANYOF(", 6)) {
            pos += 5;
            parseMesNaryArgs(tree.mutable_any_of()->mutable_terms());
            return tree;
        }
        if (startsWith("DESTINATION(", 12)) {
            pos += 12;
            skipWhitespace();
            const std::string bytes = parseUUIDBytes();
            skipWhitespace();
            expect(')');
            auto* fe = tree.mutable_field_equals();
            fe->mutable_path()->add_segments("destination");
            fe->mutable_path()->add_segments("value");
            fe->mutable_value()->set_bytes_value(bytes);
            return tree;
        }
        if (startsWith("SOURCE(", 7)) {
            pos += 7;
            skipWhitespace();
            const std::string bytes = parseUUIDBytes();
            skipWhitespace();
            expect(')');
            auto* fe = tree.mutable_field_equals();
            fe->mutable_path()->add_segments("signature");
            fe->mutable_path()->add_segments("signer");
            fe->mutable_path()->add_segments("value");
            fe->mutable_value()->set_bytes_value(bytes);
            return tree;
        }
        if (startsWith("FIELD_EQ(", 9)) {
            pos += 9;
            *tree.mutable_field_equals()->mutable_path() = parseFieldPath();
            skipWhitespace();
            expect(',');
            *tree.mutable_field_equals()->mutable_value() = parseFieldValue();
            skipWhitespace();
            expect(')');
            return tree;
        }
        if (startsWith("FIELD_EXISTS(", 13)) {
            pos += 13;
            *tree.mutable_field_exists()->mutable_path() = parseFieldPath();
            skipWhitespace();
            expect(')');
            return tree;
        }
        if (startsWith("FIELD_CONTAINS(", 15)) {
            pos += 15;
            *tree.mutable_field_contains()->mutable_path() = parseFieldPath();
            skipWhitespace();
            expect(',');
            *tree.mutable_field_contains()->mutable_sub_path() = parseFieldPath();
            skipWhitespace();
            expect(',');
            *tree.mutable_field_contains()->mutable_value() = parseFieldValue();
            skipWhitespace();
            expect(')');
            return tree;
        }
        if (startsWithCI("TRUE", 4)) {
            pos += 4;
            tree.mutable_true_value();
            return tree;
        }
        if (startsWithCI("FALSE", 5)) {
            pos += 5;
            tree.mutable_false_value();
            return tree;
        }

        throw std::runtime_error(
            "unexpected token in message sub-tree at position " + std::to_string(pos));
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
        if (startsWith("ENDORSEMENT(", 12)) {
            pos += 12;
            skipWhitespace();
            *tree.mutable_endorsement()->mutable_tree() = parseEndTree();
            skipWhitespace();
            expect(')');
            return tree;
        }
        if (startsWith("ENDORSEMENT_TYPE(", 17)) {
            pos += 17;
            skipWhitespace();
            tree.mutable_endorsement()->mutable_tree()->mutable_type_equals()->mutable_type()->set_value(parseUUIDBytes());
            skipWhitespace();
            expect(')');
            return tree;
        }
        if (startsWith("ENDORSEMENT_VALUE(", 18)) {
            pos += 18;
            skipWhitespace();
            tree.mutable_endorsement()->mutable_tree()->mutable_value_equals()->set_value(parseHexBytes());
            skipWhitespace();
            expect(')');
            return tree;
        }
        if (startsWith("ENDORSEMENT_SIGNER(", 19)) {
            pos += 19;
            skipWhitespace();
            tree.mutable_endorsement()->mutable_tree()->mutable_signer_equals()->mutable_signer()->set_value(parseUUIDBytes());
            skipWhitespace();
            expect(')');
            return tree;
        }
        if (startsWith("MESSAGE(", 8)) {
            pos += 8;
            skipWhitespace();
            *tree.mutable_message()->mutable_tree() = parseMessageTree();
            skipWhitespace();
            expect(')');
            return tree;
        }
        if (startsWith("DESTINATION(", 12)) {
            pos += 12;
            skipWhitespace();
            const std::string bytes = parseUUIDBytes();
            skipWhitespace();
            expect(')');
            auto* fe = tree.mutable_message()->mutable_tree()->mutable_field_equals();
            fe->mutable_path()->add_segments("destination");
            fe->mutable_path()->add_segments("value");
            fe->mutable_value()->set_bytes_value(bytes);
            return tree;
        }
        if (startsWith("SOURCE(", 7)) {
            pos += 7;
            skipWhitespace();
            const std::string bytes = parseUUIDBytes();
            skipWhitespace();
            expect(')');
            auto* fe = tree.mutable_message()->mutable_tree()->mutable_field_equals();
            fe->mutable_path()->add_segments("signature");
            fe->mutable_path()->add_segments("signer");
            fe->mutable_path()->add_segments("value");
            fe->mutable_value()->set_bytes_value(bytes);
            return tree;
        }
        if (startsWithCI("TRUE", 4)) {
            pos += 4;
            tree.mutable_true_value();
            return tree;
        }
        if (startsWithCI("FALSE", 5)) {
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
