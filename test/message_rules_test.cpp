#include "common/config/message_rules.hpp"
#include "common/endorsement/endorsement_text.hpp"

#include "third_party/json/single_include/nlohmann/json.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

int gFailures = 0;

#define EXPECT(cond) do {                                                        \
    if (!(cond)) {                                                               \
        std::cerr << "FAIL " << __FILE__ << ':' << __LINE__                      \
                  << ": " << #cond << '\n';                                      \
        ++gFailures;                                                             \
    }                                                                            \
} while (0)

#define EXPECT_THROWS(expr) do {                                                 \
    bool caught = false;                                                         \
    try { (void)(expr); } catch (const std::exception&) { caught = true; }       \
    if (!caught) {                                                               \
        std::cerr << "FAIL " << __FILE__ << ':' << __LINE__                      \
                  << ": expected throw from " << #expr << '\n';                  \
        ++gFailures;                                                             \
    }                                                                            \
} while (0)

#define EXPECT_THROWS_MSG(expr, needle) do {                                     \
    bool caught = false;                                                         \
    std::string what;                                                            \
    try { (void)(expr); }                                                        \
    catch (const std::exception& e) { caught = true; what = e.what(); }          \
    if (!caught) {                                                               \
        std::cerr << "FAIL " << __FILE__ << ':' << __LINE__                      \
                  << ": expected throw from " << #expr << '\n';                  \
        ++gFailures;                                                             \
    } else if (what.find(needle) == std::string::npos) {                         \
        std::cerr << "FAIL " << __FILE__ << ':' << __LINE__                      \
                  << ": exception '" << what << "' did not contain '"            \
                  << needle << "'\n";                                            \
        ++gFailures;                                                             \
    }                                                                            \
} while (0)

rsp::config::MacroContext makeMacros() {
    rsp::config::MacroContext m;
    // Use a representative 32-character lowercase hex node id (16 bytes).
    m.nodeId = "0123456789abcdef0123456789abcdef";
    return m;
}

// ---------------------------------------------------------------------------
// erd_text: ${} atom tests
// ---------------------------------------------------------------------------

void testResolverHitSplicesSubtree() {
    rsp::proto::ERDAbstractSyntaxTree value;
    value.mutable_true_value();
    rsp::erd_text::VariableResolver resolver =
        [&](const std::string& name) -> std::optional<rsp::proto::ERDAbstractSyntaxTree> {
        if (name == "x") return value;
        return std::nullopt;
    };
    auto tree = rsp::erd_text::fromString("AND(${x}, FALSE)", resolver);
    EXPECT(tree.node_type_case() == rsp::proto::ERDAbstractSyntaxTree::kAnd);
    EXPECT(tree.and_().lhs().node_type_case() == rsp::proto::ERDAbstractSyntaxTree::kTrueValue);
    EXPECT(tree.and_().rhs().node_type_case() == rsp::proto::ERDAbstractSyntaxTree::kFalseValue);
}

void testResolverMissThrows() {
    rsp::erd_text::VariableResolver resolver =
        [](const std::string&) -> std::optional<rsp::proto::ERDAbstractSyntaxTree> {
        return std::nullopt;
    };
    EXPECT_THROWS_MSG(rsp::erd_text::fromString("${foo}", resolver), "undeclared variable");
}

void testUnterminatedDollarBraceThrows() {
    rsp::erd_text::VariableResolver resolver =
        [](const std::string&) -> std::optional<rsp::proto::ERDAbstractSyntaxTree> {
        return std::nullopt;
    };
    EXPECT_THROWS_MSG(rsp::erd_text::fromString("${foo", resolver), "unterminated");
}

void testDollarBraceWithoutResolverThrows() {
    EXPECT_THROWS(rsp::erd_text::fromString("${foo}"));
}

// ---------------------------------------------------------------------------
// message_rules tests
// ---------------------------------------------------------------------------

nlohmann::json arr(std::initializer_list<const char*> entries) {
    nlohmann::json j = nlohmann::json::array();
    for (const char* e : entries) j.push_back(std::string(e));
    return j;
}

void testHappyPathMatchesHandBuilt() {
    auto j = arr({
        "// header comment",
        "",
        "let yes = TRUE",
        "let no  = FALSE",
        "# the actual rule:",
        "AND(${yes}, OR(${no}, ${yes}))",
    });
    auto tree = rsp::config::buildMessageRulesTree(j, makeMacros());

    rsp::proto::ERDAbstractSyntaxTree expected;
    auto* andN = expected.mutable_and_();
    andN->mutable_lhs()->mutable_true_value();
    auto* orN = andN->mutable_rhs()->mutable_or_();
    orN->mutable_lhs()->mutable_false_value();
    orN->mutable_rhs()->mutable_true_value();
    EXPECT(tree.SerializeAsString() == expected.SerializeAsString());
}

void testCommentsAndBlankSkipped() {
    auto j = arr({"", "  ", "# c", "// c2", "TRUE"});
    auto tree = rsp::config::buildMessageRulesTree(j, makeMacros());
    EXPECT(tree.node_type_case() == rsp::proto::ERDAbstractSyntaxTree::kTrueValue);
}

void testLetReferencingPriorLet() {
    auto j = arr({
        "let a = TRUE",
        "let b = AND(${a}, FALSE)",
        "${b}",
    });
    auto tree = rsp::config::buildMessageRulesTree(j, makeMacros());
    EXPECT(tree.node_type_case() == rsp::proto::ERDAbstractSyntaxTree::kAnd);
    EXPECT(tree.and_().lhs().node_type_case() == rsp::proto::ERDAbstractSyntaxTree::kTrueValue);
    EXPECT(tree.and_().rhs().node_type_case() == rsp::proto::ERDAbstractSyntaxTree::kFalseValue);
}

void testDuplicateVariableRejected() {
    auto j = arr({
        "let a = TRUE",
        "let a = FALSE",
        "${a}",
    });
    EXPECT_THROWS_MSG(rsp::config::buildMessageRulesTree(j, makeMacros()), "duplicate variable");
}

void testUndeclaredReferenceRejected() {
    auto j = arr({"${missing}"});
    EXPECT_THROWS_MSG(rsp::config::buildMessageRulesTree(j, makeMacros()),
                      "undeclared variable");
}

void testUnterminatedDollarBraceRejected() {
    auto j = arr({"AND(${a, TRUE)"});
    EXPECT_THROWS_MSG(rsp::config::buildMessageRulesTree(j, makeMacros()), "unterminated");
}

void testZeroRuleLinesRejected() {
    auto j = arr({"// only comments", "let a = TRUE"});
    EXPECT_THROWS_MSG(rsp::config::buildMessageRulesTree(j, makeMacros()),
                      "exactly one rule line");
}

void testTwoRuleLinesRejected() {
    auto j = arr({"TRUE", "FALSE"});
    EXPECT_THROWS_MSG(rsp::config::buildMessageRulesTree(j, makeMacros()),
                      "rule line must be last");
}

void testLetAfterRuleLineRejected() {
    auto j = arr({"TRUE", "let a = FALSE"});
    EXPECT_THROWS_MSG(rsp::config::buildMessageRulesTree(j, makeMacros()),
                      "rule line must be last");
}

void testMyNodeIdMacroInsideLet() {
    auto j = arr({
        "let me = SOURCE(01234567-89ab-cdef-0123-456789abcdef)",
        "${me}",
    });
    auto tree = rsp::config::buildMessageRulesTree(j, makeMacros());
    EXPECT(tree.node_type_case() == rsp::proto::ERDAbstractSyntaxTree::kMessage);
}

void testInvalidIdentifierRejected() {
    auto j = arr({"let 1bad = TRUE", "${1bad}"});
    EXPECT_THROWS_MSG(rsp::config::buildMessageRulesTree(j, makeMacros()),
                      "invalid variable name");
}

void testLetWithoutEqualsRejected() {
    auto j = arr({"let a TRUE", "${a}"});
    EXPECT_THROWS_MSG(rsp::config::buildMessageRulesTree(j, makeMacros()), "missing '='");
}

void testEmptyArrayRejected() {
    auto j = nlohmann::json::array();
    EXPECT_THROWS_MSG(rsp::config::buildMessageRulesTree(j, makeMacros()),
                      "exactly one rule line");
}

void testNonStringEntryRejected() {
    nlohmann::json j = nlohmann::json::array();
    j.push_back(42);
    EXPECT_THROWS_MSG(rsp::config::buildMessageRulesTree(j, makeMacros()), "must be a string");
}

void testBackwardsCompatSingleTrue() {
    auto j = arr({"true"});
    auto tree = rsp::config::buildMessageRulesTree(j, makeMacros());
    EXPECT(tree.node_type_case() == rsp::proto::ERDAbstractSyntaxTree::kTrueValue);
}

}  // namespace

int main() {
    testResolverHitSplicesSubtree();
    testResolverMissThrows();
    testUnterminatedDollarBraceThrows();
    testDollarBraceWithoutResolverThrows();

    testHappyPathMatchesHandBuilt();
    testCommentsAndBlankSkipped();
    testLetReferencingPriorLet();
    testDuplicateVariableRejected();
    testUndeclaredReferenceRejected();
    testUnterminatedDollarBraceRejected();
    testZeroRuleLinesRejected();
    testTwoRuleLinesRejected();
    testLetAfterRuleLineRejected();
    testMyNodeIdMacroInsideLet();
    testInvalidIdentifierRejected();
    testLetWithoutEqualsRejected();
    testEmptyArrayRejected();
    testNonStringEntryRejected();
    testBackwardsCompatSingleTrue();

    if (gFailures != 0) {
        std::cerr << gFailures << " message_rules test(s) failed\n";
        return 1;
    }
    std::cout << "message_rules: all tests passed\n";
    return 0;
}
