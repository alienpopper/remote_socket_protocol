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

rsp::proto::ERDAbstractSyntaxTree makeEndorsementTypeEquals(const std::string& uuidStr) {
    rsp::proto::ERDAbstractSyntaxTree tree;
    tree.mutable_endorsement()->mutable_tree()->mutable_type_equals()->mutable_type()->set_value(uuidBytes(uuidStr));
    return tree;
}

rsp::proto::ERDAbstractSyntaxTree makeEndorsementValueEquals(const std::string& bytes) {
    rsp::proto::ERDAbstractSyntaxTree tree;
    tree.mutable_endorsement()->mutable_tree()->mutable_value_equals()->set_value(bytes);
    return tree;
}

rsp::proto::ERDAbstractSyntaxTree makeEndorsementSignerEquals(const std::string& uuidStr) {
    rsp::proto::ERDAbstractSyntaxTree tree;
    tree.mutable_endorsement()->mutable_tree()->mutable_signer_equals()->mutable_signer()->set_value(uuidBytes(uuidStr));
    return tree;
}

rsp::proto::ERDAbstractSyntaxTree makeDestinationEquals(const std::string& uuidStr) {
    rsp::proto::ERDAbstractSyntaxTree tree;
    auto* fe = tree.mutable_message()->mutable_tree()->mutable_field_equals();
    fe->mutable_path()->add_segments("destination");
    fe->mutable_path()->add_segments("value");
    fe->mutable_value()->set_bytes_value(uuidBytes(uuidStr));
    return tree;
}

rsp::proto::ERDAbstractSyntaxTree makeMessageSourceEquals(const std::string& uuidStr) {
    rsp::proto::ERDAbstractSyntaxTree tree;
    auto* fe = tree.mutable_message()->mutable_tree()->mutable_field_equals();
    fe->mutable_path()->add_segments("signature");
    fe->mutable_path()->add_segments("signer");
    fe->mutable_path()->add_segments("value");
    fe->mutable_value()->set_bytes_value(uuidBytes(uuidStr));
    return tree;
}

rsp::proto::ERDAbstractSyntaxTree makeTrue() {
    rsp::proto::ERDAbstractSyntaxTree tree;
    tree.mutable_true_value();
    return tree;
}

rsp::proto::ERDAbstractSyntaxTree makeFalse() {
    rsp::proto::ERDAbstractSyntaxTree tree;
    tree.mutable_false_value();
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

rsp::proto::ERDAbstractSyntaxTree makeAllOf(
    std::initializer_list<rsp::proto::ERDAbstractSyntaxTree> terms) {
    rsp::proto::ERDAbstractSyntaxTree tree;
    for (const auto& term : terms) {
        *tree.mutable_all_of()->add_terms() = term;
    }
    return tree;
}

rsp::proto::ERDAbstractSyntaxTree makeAnyOf(
    std::initializer_list<rsp::proto::ERDAbstractSyntaxTree> terms) {
    rsp::proto::ERDAbstractSyntaxTree tree;
    for (const auto& term : terms) {
        *tree.mutable_any_of()->add_terms() = term;
    }
    return tree;
}

void testRoundTripEndorsementTypeEquals() {
    const std::string uuid = "00112233-4455-6677-8899-aabbccddeeff";
    const auto tree = makeEndorsementTypeEquals(uuid);
    const std::string text = rsp::erd_text::toString(tree);
    require(text == "ENDORSEMENT(ENDORSEMENT_TYPE(00112233-4455-6677-8899-aabbccddeeff))", "ENDORSEMENT_TYPE text mismatch: " + text);

    const auto parsed = rsp::erd_text::fromString(text);
    require(parsed.has_endorsement(), "parsed should have endorsement");
    require(parsed.endorsement().tree().type_equals().type().value() == uuidBytes(uuid), "ENDORSEMENT_TYPE uuid bytes mismatch");
}

void testRoundTripEndorsementValueEquals() {
    rsp::proto::ERDAbstractSyntaxTree tree;
    tree.mutable_endorsement()->mutable_tree()->mutable_value_equals()->set_value(std::string("\xde\xad\xbe\xef", 4));
    const std::string text = rsp::erd_text::toString(tree);
    require(text == "ENDORSEMENT(ENDORSEMENT_VALUE(deadbeef))", "ENDORSEMENT_VALUE text mismatch: " + text);

    const auto parsed = rsp::erd_text::fromString(text);
    require(parsed.has_endorsement(), "parsed should have endorsement");
    require(parsed.endorsement().tree().value_equals().value() == std::string("\xde\xad\xbe\xef", 4),
            "ENDORSEMENT_VALUE bytes mismatch");
}

void testRoundTripEndorsementValueEqualsEmpty() {
    rsp::proto::ERDAbstractSyntaxTree tree;
    tree.mutable_endorsement()->mutable_tree()->mutable_value_equals()->set_value("");
    const std::string text = rsp::erd_text::toString(tree);
    require(text == "ENDORSEMENT(ENDORSEMENT_VALUE())", "empty ENDORSEMENT_VALUE text mismatch: " + text);

    const auto parsed = rsp::erd_text::fromString(text);
    require(parsed.has_endorsement(), "parsed should have endorsement");
    require(parsed.endorsement().tree().value_equals().value().empty(), "ENDORSEMENT_VALUE should be empty");
}

void testRoundTripEndorsementSignerEquals() {
    const std::string uuid = "aabbccdd-eeff-0011-2233-445566778899";
    const auto tree = makeEndorsementSignerEquals(uuid);
    const std::string text = rsp::erd_text::toString(tree);
    require(text == "ENDORSEMENT(ENDORSEMENT_SIGNER(aabbccdd-eeff-0011-2233-445566778899))", "ENDORSEMENT_SIGNER text mismatch: " + text);

    const auto parsed = rsp::erd_text::fromString(text);
    require(parsed.has_endorsement(), "parsed should have endorsement");
    require(parsed.endorsement().tree().signer_equals().signer().value() == uuidBytes(uuid), "ENDORSEMENT_SIGNER uuid bytes mismatch");
}

void testRoundTripDestinationEquals() {
    const std::string uuid = "12345678-90ab-cdef-0123-456789abcdef";
    const auto tree = makeDestinationEquals(uuid);
    const std::string text = rsp::erd_text::toString(tree);
    require(text == "MESSAGE(FIELD_EQ(destination.value, 0x1234567890abcdef0123456789abcdef))",
            "DESTINATION text mismatch: " + text);

    const auto parsed = rsp::erd_text::fromString(text);
    require(parsed.has_message(), "parsed should have message");
    require(parsed.message().tree().has_field_equals(), "parsed message tree should be field_equals");
    require(parsed.message().tree().field_equals().value().bytes_value() == uuidBytes(uuid),
            "DESTINATION uuid bytes mismatch");
}

void testRoundTripMessageSourceEquals() {
    const std::string uuid = "fedcba98-7654-3210-fedc-ba9876543210";
    const auto tree = makeMessageSourceEquals(uuid);
    const std::string text = rsp::erd_text::toString(tree);
    require(text == "MESSAGE(FIELD_EQ(signature.signer.value, 0xfedcba9876543210fedcba9876543210))",
            "SOURCE text mismatch: " + text);

    const auto parsed = rsp::erd_text::fromString(text);
    require(parsed.has_message(), "parsed should have message");
    require(parsed.message().tree().has_field_equals(), "parsed message tree should be field_equals");
    require(parsed.message().tree().field_equals().value().bytes_value() == uuidBytes(uuid),
            "SOURCE uuid bytes mismatch");
}

void testRoundTripTrue() {
    const auto tree = makeTrue();
    const std::string text = rsp::erd_text::toString(tree);
    require(text == "TRUE", "TRUE text mismatch: " + text);

    const auto parsed = rsp::erd_text::fromString(text);
    require(parsed.has_true_value(), "parsed should have true_value");
}

void testRoundTripFalse() {
    const auto tree = makeFalse();
    const std::string text = rsp::erd_text::toString(tree);
    require(text == "FALSE", "FALSE text mismatch: " + text);

    const auto parsed = rsp::erd_text::fromString(text);
    require(parsed.has_false_value(), "parsed should have false_value");
}

void testRoundTripAllOf() {
    const auto tree = makeAllOf({
        makeEndorsementTypeEquals("11111111-1111-1111-1111-111111111111"),
        makeTrue(),
        makeEndorsementSignerEquals("22222222-2222-2222-2222-222222222222"),
    });
    const std::string text = rsp::erd_text::toString(tree);
    require(text ==
                "ALLOF(ENDORSEMENT(ENDORSEMENT_TYPE(11111111-1111-1111-1111-111111111111)), TRUE, "
                "ENDORSEMENT(ENDORSEMENT_SIGNER(22222222-2222-2222-2222-222222222222)))",
            "ALLOF text mismatch: " + text);

    const auto parsed = rsp::erd_text::fromString(text);
    require(parsed.has_all_of(), "parsed should have all_of");
    require(parsed.all_of().terms_size() == 3, "ALLOF should preserve term count");
    require(parsed.all_of().terms(1).has_true_value(), "ALLOF second term should be TRUE");
}

void testRoundTripAnyOf() {
    const auto tree = makeAnyOf({
        makeFalse(),
        makeEndorsementValueEquals(std::string("\xca\xfe\xba\xbe", 4)),
        makeEndorsementTypeEquals("33333333-3333-3333-3333-333333333333"),
    });
    const std::string text = rsp::erd_text::toString(tree);
    require(text ==
                "ANYOF(FALSE, ENDORSEMENT(ENDORSEMENT_VALUE(cafebabe)), ENDORSEMENT(ENDORSEMENT_TYPE(33333333-3333-3333-3333-333333333333)))",
            "ANYOF text mismatch: " + text);

    const auto parsed = rsp::erd_text::fromString(text);
    require(parsed.has_any_of(), "parsed should have any_of");
    require(parsed.any_of().terms_size() == 3, "ANYOF should preserve term count");
    require(parsed.any_of().terms(0).has_false_value(), "ANYOF first term should be FALSE");
}

void testRoundTripAND() {
    const auto tree = makeAnd(makeEndorsementTypeEquals("11111111-1111-1111-1111-111111111111"),
                              makeEndorsementTypeEquals("22222222-2222-2222-2222-222222222222"));
    const std::string text = rsp::erd_text::toString(tree);
    require(text == "AND(ENDORSEMENT(ENDORSEMENT_TYPE(11111111-1111-1111-1111-111111111111)), "
                    "ENDORSEMENT(ENDORSEMENT_TYPE(22222222-2222-2222-2222-222222222222)))",
            "AND text mismatch: " + text);

    const auto parsed = rsp::erd_text::fromString(text);
    require(parsed.has_and_(), "parsed should have and_");
    require(parsed.and_().lhs().has_endorsement(), "AND lhs should be ENDORSEMENT");
    require(parsed.and_().rhs().has_endorsement(), "AND rhs should be ENDORSEMENT");
}

void testRoundTripOR() {
    const auto tree = makeOr(makeEndorsementSignerEquals("aaaaaaaa-0000-0000-0000-000000000001"),
                                 makeEndorsementTypeEquals("bbbbbbbb-0000-0000-0000-000000000002"));
    const std::string text = rsp::erd_text::toString(tree);
    require(text == "OR(ENDORSEMENT(ENDORSEMENT_SIGNER(aaaaaaaa-0000-0000-0000-000000000001)), "
                            "ENDORSEMENT(ENDORSEMENT_TYPE(bbbbbbbb-0000-0000-0000-000000000002)))",
            "OR text mismatch: " + text);

    const auto parsed = rsp::erd_text::fromString(text);
    require(parsed.has_or_(), "parsed should have or_");
    require(parsed.or_().lhs().has_endorsement(), "OR lhs should be ENDORSEMENT");
    require(parsed.or_().rhs().has_endorsement(), "OR rhs should be ENDORSEMENT");
}

void testRoundTripEQ() {
    const auto tree = makeEq(makeEndorsementTypeEquals("11111111-0000-0000-0000-000000000001"),
                             makeEndorsementTypeEquals("22222222-0000-0000-0000-000000000002"));
    const std::string text = rsp::erd_text::toString(tree);
    require(text == "EQ(ENDORSEMENT(ENDORSEMENT_TYPE(11111111-0000-0000-0000-000000000001)), "
                    "ENDORSEMENT(ENDORSEMENT_TYPE(22222222-0000-0000-0000-000000000002)))",
            "EQ text mismatch: " + text);

    const auto parsed = rsp::erd_text::fromString(text);
    require(parsed.has_equals(), "parsed should have equals");
}

void testRoundTripNested() {
    // AND(OR(ENDORSEMENT_TYPE(t1), ENDORSEMENT_SIGNER(s1)), ENDORSEMENT_VALUE(cafebabe))
    const auto tree = makeAnd(
        makeOr(makeEndorsementTypeEquals("11111111-1111-1111-1111-111111111111"),
               makeEndorsementSignerEquals("22222222-2222-2222-2222-222222222222")),
        makeEndorsementValueEquals(std::string("\xca\xfe\xba\xbe", 4)));

    const std::string text = rsp::erd_text::toString(tree);
    const std::string expected =
        "AND("
        "OR(ENDORSEMENT(ENDORSEMENT_TYPE(11111111-1111-1111-1111-111111111111)), ENDORSEMENT(ENDORSEMENT_SIGNER(22222222-2222-2222-2222-222222222222))), "
        "ENDORSEMENT(ENDORSEMENT_VALUE(cafebabe))"
        ")";
    require(text == expected, "nested text mismatch: " + text);

    const auto parsed = rsp::erd_text::fromString(text);
    require(parsed.has_and_(), "root should be AND");
    require(parsed.and_().lhs().has_or_(), "AND lhs should be OR");
        require(parsed.and_().lhs().or_().lhs().has_endorsement(), "OR lhs should be ENDORSEMENT");
    require(parsed.and_().lhs().or_().rhs().has_endorsement(), "OR rhs should be ENDORSEMENT");
        require(parsed.and_().rhs().has_endorsement(), "AND rhs should be ENDORSEMENT");
        require(parsed.and_().rhs().endorsement().tree().value_equals().value() == std::string("\xca\xfe\xba\xbe", 4),
            "ENDORSEMENT_VALUE bytes mismatch");
}

void testEmptyTree() {
    const rsp::proto::ERDAbstractSyntaxTree empty;
    require(rsp::erd_text::toString(empty).empty(), "empty tree should produce empty string");

    const auto parsed = rsp::erd_text::fromString("");
    require(!parsed.has_and_() && !parsed.has_or_() && !parsed.has_equals() &&
                !parsed.has_endorsement() && !parsed.has_message(),
            "empty string should parse to unset tree");

    const auto parsedWhitespace = rsp::erd_text::fromString("   \t\n  ");
    require(parsedWhitespace.node_type_case() ==
                rsp::proto::ERDAbstractSyntaxTree::NODE_TYPE_NOT_SET,
            "whitespace should parse to unset tree");

    require(rsp::erd_text::fromString(" TRUE ").has_true_value(),
            "TRUE with surrounding whitespace should parse");
    require(rsp::erd_text::fromString(" FALSE \n").has_false_value(),
            "FALSE with surrounding whitespace should parse");
}

void testParseWhitespaceTolerance() {
    const std::string text =
        "AND( \n"
        "  ENDORSEMENT_TYPE( 11111111-1111-1111-1111-111111111111 ) ,\n"
        "  OR( ENDORSEMENT_TYPE( 22222222-2222-2222-2222-222222222222 ) ,\n"
        "      ENDORSEMENT_SIGNER( 33333333-3333-3333-3333-333333333333 ) ) )";
    const auto parsed = rsp::erd_text::fromString(text);
    require(parsed.has_and_(), "should be AND");
    require(parsed.and_().lhs().has_endorsement(), "AND lhs should be ENDORSEMENT");
    require(parsed.and_().rhs().has_or_(), "AND rhs should be OR");
}

void testParseErrorUnknownToken() {
    bool threw = false;
    try { rsp::erd_text::fromString("UNKNOWN(foo)"); } catch (const std::runtime_error&) { threw = true; }
    require(threw, "unknown token should throw");
}

void testParseErrorMalformedUUID() {
    bool threw = false;
    try { rsp::erd_text::fromString("ENDORSEMENT_TYPE(not-a-uuid)"); } catch (const std::runtime_error&) { threw = true; }
    require(threw, "malformed UUID should throw");
}

void testParseErrorOddHex() {
    bool threw = false;
    try { rsp::erd_text::fromString("ENDORSEMENT_VALUE(abc)"); } catch (const std::runtime_error&) { threw = true; }
    require(threw, "odd hex count should throw");
}

void testParseErrorMissingComma() {
    bool threw = false;
    try { rsp::erd_text::fromString("AND(ENDORSEMENT_TYPE(11111111-1111-1111-1111-111111111111) ENDORSEMENT_TYPE(22222222-2222-2222-2222-222222222222))"); }
    catch (const std::runtime_error&) { threw = true; }
    require(threw, "missing comma should throw");
}

void testParseErrorUnclosedParen() {
    bool threw = false;
    try { rsp::erd_text::fromString("AND(ENDORSEMENT_TYPE(11111111-1111-1111-1111-111111111111), ENDORSEMENT_TYPE(22222222-2222-2222-2222-222222222222)"); }
    catch (const std::runtime_error&) { threw = true; }
    require(threw, "unclosed paren should throw");
}

void testParseErrorTrailingContent() {
    bool threw = false;
    try { rsp::erd_text::fromString("ENDORSEMENT_TYPE(11111111-1111-1111-1111-111111111111) garbage"); }
    catch (const std::runtime_error&) { threw = true; }
    require(threw, "trailing content should throw");
}

}  // namespace

int main() {
    try {
        testRoundTripEndorsementTypeEquals();
        testRoundTripEndorsementValueEquals();
        testRoundTripEndorsementValueEqualsEmpty();
        testRoundTripEndorsementSignerEquals();
        testRoundTripDestinationEquals();
        testRoundTripMessageSourceEquals();
        testRoundTripTrue();
        testRoundTripFalse();
        testRoundTripAllOf();
        testRoundTripAnyOf();
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
