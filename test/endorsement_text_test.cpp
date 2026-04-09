#include "common/endorsement/endorsement_text.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

// Build a 16-byte big-endian UUID bytes string from a UUID string.
std::string uuidBytes(const std::string& uuidStr) {
    std::string hex;
    for (char c : uuidStr) {
        if (c != '-') hex += c;
    }
    auto hexVal = [](char c) -> uint8_t {
        if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
        if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
        if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(c - 'A' + 10);
        return 0;
    };
    std::string result;
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        result += static_cast<char>((hexVal(hex[i]) << 4) | hexVal(hex[i + 1]));
    }
    return result;
}

rsp::proto::ERDAbstractSyntaxTree makeTypeEquals(const std::string& uuidStr) {
    rsp::proto::ERDAbstractSyntaxTree tree;
    tree.mutable_type_equals()->mutable_type()->set_value(uuidBytes(uuidStr));
    return tree;
}

rsp::proto::ERDAbstractSyntaxTree makeValueEquals(const std::string& bytes) {
    rsp::proto::ERDAbstractSyntaxTree tree;
    tree.mutable_value_equals()->set_value(bytes);
    return tree;
}

rsp::proto::ERDAbstractSyntaxTree makeSignerEquals(const std::string& uuidStr) {
    rsp::proto::ERDAbstractSyntaxTree tree;
    tree.mutable_signer_equals()->mutable_signer()->set_value(uuidBytes(uuidStr));
    return tree;
}

rsp::proto::ERDAbstractSyntaxTree makeAnd(rsp::proto::ERDAbstractSyntaxTree lhs,
                                          rsp::proto::ERDAbstractSyntaxTree rhs) {
    rsp::proto::ERDAbstractSyntaxTree tree;
    *tree.mutable_and_()->mutable_lhs() = std::move(lhs);
    *tree.mutable_and_()->mutable_rhs() = std::move(rhs);
    return tree;
}

rsp::proto::ERDAbstractSyntaxTree makeOr(rsp::proto::ERDAbstractSyntaxTree lhs,
                                         rsp::proto::ERDAbstractSyntaxTree rhs) {
    rsp::proto::ERDAbstractSyntaxTree tree;
    *tree.mutable_or_()->mutable_lhs() = std::move(lhs);
    *tree.mutable_or_()->mutable_rhs() = std::move(rhs);
    return tree;
}

rsp::proto::ERDAbstractSyntaxTree makeEq(rsp::proto::ERDAbstractSyntaxTree lhs,
                                         rsp::proto::ERDAbstractSyntaxTree rhs) {
    rsp::proto::ERDAbstractSyntaxTree tree;
    *tree.mutable_equals()->mutable_lhs() = std::move(lhs);
    *tree.mutable_equals()->mutable_rhs() = std::move(rhs);
    return tree;
}

void testRoundTripTypeEquals() {
    const std::string uuid = "00112233-4455-6677-8899-aabbccddeeff";
    const auto tree = makeTypeEquals(uuid);
    const std::string text = rsp::erd_text::toString(tree);
    require(text == "TYPE(00112233-4455-6677-8899-aabbccddeeff)", "TYPE text mismatch: " + text);

    const auto parsed = rsp::erd_text::fromString(text);
    require(parsed.has_type_equals(), "parsed should have type_equals");
    require(parsed.type_equals().type().value() == uuidBytes(uuid), "TYPE uuid bytes mismatch");
}

void testRoundTripValueEquals() {
    rsp::proto::ERDAbstractSyntaxTree tree;
    tree.mutable_value_equals()->set_value(std::string("\xde\xad\xbe\xef", 4));
    const std::string text = rsp::erd_text::toString(tree);
    require(text == "VALUE(deadbeef)", "VALUE text mismatch: " + text);

    const auto parsed = rsp::erd_text::fromString(text);
    require(parsed.has_value_equals(), "parsed should have value_equals");
    require(parsed.value_equals().value() == std::string("\xde\xad\xbe\xef", 4),
            "VALUE bytes mismatch");
}

void testRoundTripValueEqualsEmpty() {
    rsp::proto::ERDAbstractSyntaxTree tree;
    tree.mutable_value_equals()->set_value("");
    const std::string text = rsp::erd_text::toString(tree);
    require(text == "VALUE()", "empty VALUE text mismatch: " + text);

    const auto parsed = rsp::erd_text::fromString(text);
    require(parsed.has_value_equals(), "parsed should have value_equals");
    require(parsed.value_equals().value().empty(), "VALUE should be empty");
}

void testRoundTripSignerEquals() {
    const std::string uuid = "aabbccdd-eeff-0011-2233-445566778899";
    const auto tree = makeSignerEquals(uuid);
    const std::string text = rsp::erd_text::toString(tree);
    require(text == "SIGNER(aabbccdd-eeff-0011-2233-445566778899)", "SIGNER text mismatch: " + text);

    const auto parsed = rsp::erd_text::fromString(text);
    require(parsed.has_signer_equals(), "parsed should have signer_equals");
    require(parsed.signer_equals().signer().value() == uuidBytes(uuid), "SIGNER uuid bytes mismatch");
}

void testRoundTripAND() {
    const auto tree = makeAnd(makeTypeEquals("11111111-1111-1111-1111-111111111111"),
                              makeTypeEquals("22222222-2222-2222-2222-222222222222"));
    const std::string text = rsp::erd_text::toString(tree);
    require(text == "AND(TYPE(11111111-1111-1111-1111-111111111111), "
                    "TYPE(22222222-2222-2222-2222-222222222222))",
            "AND text mismatch: " + text);

    const auto parsed = rsp::erd_text::fromString(text);
    require(parsed.has_and_(), "parsed should have and_");
    require(parsed.and_().lhs().has_type_equals(), "AND lhs should be TYPE");
    require(parsed.and_().rhs().has_type_equals(), "AND rhs should be TYPE");
}

void testRoundTripOR() {
    const auto tree = makeOr(makeSignerEquals("aaaaaaaa-0000-0000-0000-000000000001"),
                             makeTypeEquals("bbbbbbbb-0000-0000-0000-000000000002"));
    const std::string text = rsp::erd_text::toString(tree);
    require(text == "OR(SIGNER(aaaaaaaa-0000-0000-0000-000000000001), "
                    "TYPE(bbbbbbbb-0000-0000-0000-000000000002))",
            "OR text mismatch: " + text);

    const auto parsed = rsp::erd_text::fromString(text);
    require(parsed.has_or_(), "parsed should have or_");
    require(parsed.or_().lhs().has_signer_equals(), "OR lhs should be SIGNER");
    require(parsed.or_().rhs().has_type_equals(), "OR rhs should be TYPE");
}

void testRoundTripEQ() {
    const auto tree = makeEq(makeTypeEquals("11111111-0000-0000-0000-000000000001"),
                             makeTypeEquals("22222222-0000-0000-0000-000000000002"));
    const std::string text = rsp::erd_text::toString(tree);
    require(text == "EQ(TYPE(11111111-0000-0000-0000-000000000001), "
                    "TYPE(22222222-0000-0000-0000-000000000002))",
            "EQ text mismatch: " + text);

    const auto parsed = rsp::erd_text::fromString(text);
    require(parsed.has_equals(), "parsed should have equals");
}

void testRoundTripNested() {
    // AND(OR(TYPE(t1), SIGNER(s1)), VALUE(cafebabe))
    const auto tree = makeAnd(
        makeOr(makeTypeEquals("11111111-1111-1111-1111-111111111111"),
               makeSignerEquals("22222222-2222-2222-2222-222222222222")),
        makeValueEquals(std::string("\xca\xfe\xba\xbe", 4)));

    const std::string text = rsp::erd_text::toString(tree);
    const std::string expected =
        "AND("
        "OR(TYPE(11111111-1111-1111-1111-111111111111), SIGNER(22222222-2222-2222-2222-222222222222)), "
        "VALUE(cafebabe)"
        ")";
    require(text == expected, "nested text mismatch: " + text);

    const auto parsed = rsp::erd_text::fromString(text);
    require(parsed.has_and_(), "root should be AND");
    require(parsed.and_().lhs().has_or_(), "AND lhs should be OR");
    require(parsed.and_().lhs().or_().lhs().has_type_equals(), "OR lhs should be TYPE");
    require(parsed.and_().lhs().or_().rhs().has_signer_equals(), "OR rhs should be SIGNER");
    require(parsed.and_().rhs().has_value_equals(), "AND rhs should be VALUE");
    require(parsed.and_().rhs().value_equals().value() == std::string("\xca\xfe\xba\xbe", 4),
            "VALUE bytes mismatch");
}

void testEmptyTree() {
    const rsp::proto::ERDAbstractSyntaxTree empty;
    require(rsp::erd_text::toString(empty).empty(), "empty tree should produce empty string");

    const auto parsed = rsp::erd_text::fromString("");
    require(!parsed.has_and_() && !parsed.has_or_() && !parsed.has_equals() &&
                !parsed.has_type_equals() && !parsed.has_value_equals() &&
                !parsed.has_signer_equals(),
            "empty string should parse to unset tree");

    const auto parsedWhitespace = rsp::erd_text::fromString("   \t\n  ");
    require(parsedWhitespace.node_type_case() ==
                rsp::proto::ERDAbstractSyntaxTree::NODE_TYPE_NOT_SET,
            "whitespace should parse to unset tree");
}

void testParseWhitespaceTolerance() {
    const std::string text =
        "AND( \n"
        "  TYPE( 11111111-1111-1111-1111-111111111111 ) ,\n"
        "  OR( TYPE( 22222222-2222-2222-2222-222222222222 ) ,\n"
        "      SIGNER( 33333333-3333-3333-3333-333333333333 ) ) )";
    const auto parsed = rsp::erd_text::fromString(text);
    require(parsed.has_and_(), "should be AND");
    require(parsed.and_().lhs().has_type_equals(), "AND lhs should be TYPE");
    require(parsed.and_().rhs().has_or_(), "AND rhs should be OR");
}

void testParseErrorUnknownToken() {
    bool threw = false;
    try { rsp::erd_text::fromString("UNKNOWN(foo)"); } catch (const std::runtime_error&) { threw = true; }
    require(threw, "unknown token should throw");
}

void testParseErrorMalformedUUID() {
    bool threw = false;
    try { rsp::erd_text::fromString("TYPE(not-a-uuid)"); } catch (const std::runtime_error&) { threw = true; }
    require(threw, "malformed UUID should throw");
}

void testParseErrorOddHex() {
    bool threw = false;
    try { rsp::erd_text::fromString("VALUE(abc)"); } catch (const std::runtime_error&) { threw = true; }
    require(threw, "odd hex count should throw");
}

void testParseErrorMissingComma() {
    bool threw = false;
    try { rsp::erd_text::fromString("AND(TYPE(11111111-1111-1111-1111-111111111111) TYPE(22222222-2222-2222-2222-222222222222))"); }
    catch (const std::runtime_error&) { threw = true; }
    require(threw, "missing comma should throw");
}

void testParseErrorUnclosedParen() {
    bool threw = false;
    try { rsp::erd_text::fromString("AND(TYPE(11111111-1111-1111-1111-111111111111), TYPE(22222222-2222-2222-2222-222222222222)"); }
    catch (const std::runtime_error&) { threw = true; }
    require(threw, "unclosed paren should throw");
}

void testParseErrorTrailingContent() {
    bool threw = false;
    try { rsp::erd_text::fromString("TYPE(11111111-1111-1111-1111-111111111111) garbage"); }
    catch (const std::runtime_error&) { threw = true; }
    require(threw, "trailing content should throw");
}

}  // namespace

int main() {
    try {
        testRoundTripTypeEquals();
        testRoundTripValueEquals();
        testRoundTripValueEqualsEmpty();
        testRoundTripSignerEquals();
        testRoundTripAND();
        testRoundTripOR();
        testRoundTripEQ();
        testRoundTripNested();
        testEmptyTree();
        testParseWhitespaceTolerance();
        testParseErrorUnknownToken();
        testParseErrorMalformedUUID();
        testParseErrorOddHex();
        testParseErrorMissingComma();
        testParseErrorUnclosedParen();
        testParseErrorTrailingContent();
        std::cout << "endorsement_text_test passed" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "endorsement_text_test failed: " << e.what() << std::endl;
        return 1;
    }
}
