#include "common/endorsement/endorsement.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <iostream>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

rsp::proto::NodeId toProtoNodeId(const rsp::NodeID& nodeId) {
    rsp::proto::NodeId protoNodeId;
    std::string value;
    value.reserve(16);
    for (int shift = 56; shift >= 0; shift -= 8) {
        value.push_back(static_cast<char>((nodeId.high() >> shift) & 0xFFULL));
    }
    for (int shift = 56; shift >= 0; shift -= 8) {
        value.push_back(static_cast<char>((nodeId.low() >> shift) & 0xFFULL));
    }
    protoNodeId.set_value(value);
    return protoNodeId;
}

rsp::proto::EndorsementType toProtoEndorsementType(const rsp::GUID& endorsementType) {
    rsp::proto::EndorsementType protoEndorsementType;
    std::string value;
    value.reserve(16);
    for (int shift = 56; shift >= 0; shift -= 8) {
        value.push_back(static_cast<char>((endorsementType.high() >> shift) & 0xFFULL));
    }
    for (int shift = 56; shift >= 0; shift -= 8) {
        value.push_back(static_cast<char>((endorsementType.low() >> shift) & 0xFFULL));
    }
    protoEndorsementType.set_value(value);
    return protoEndorsementType;
}

rsp::proto::ERDAbstractSyntaxTree makeEndorsementTypeEqualsTree(const rsp::GUID& endorsementType) {
    rsp::proto::ERDAbstractSyntaxTree tree;
    *tree.mutable_endorsement_type_equals()->mutable_type() = toProtoEndorsementType(endorsementType);
    return tree;
}

rsp::proto::ERDAbstractSyntaxTree makeEndorsementValueEqualsTree(const std::string& value) {
    rsp::proto::ERDAbstractSyntaxTree tree;
    tree.mutable_endorsement_value_equals()->set_value(value);
    return tree;
}

rsp::proto::ERDAbstractSyntaxTree makeEndorsementSignerEqualsTree(const rsp::NodeID& signer) {
    rsp::proto::ERDAbstractSyntaxTree tree;
    *tree.mutable_endorsement_signer_equals()->mutable_signer() = toProtoNodeId(signer);
    return tree;
}

rsp::proto::ERDAbstractSyntaxTree makeMessageDestinationTree(const rsp::NodeID& destination) {
    rsp::proto::ERDAbstractSyntaxTree tree;
    *tree.mutable_message_destination()->mutable_destination() = toProtoNodeId(destination);
    return tree;
}

rsp::proto::ERDAbstractSyntaxTree makeMessageSourceTree(const rsp::NodeID& source) {
    rsp::proto::ERDAbstractSyntaxTree tree;
    *tree.mutable_message_source()->mutable_source() = toProtoNodeId(source);
    return tree;
}

rsp::proto::ERDAbstractSyntaxTree makeTrueTree() {
    rsp::proto::ERDAbstractSyntaxTree tree;
    tree.mutable_true_value();
    return tree;
}

rsp::proto::ERDAbstractSyntaxTree makeFalseTree() {
    rsp::proto::ERDAbstractSyntaxTree tree;
    tree.mutable_false_value();
    return tree;
}

rsp::proto::ERDAbstractSyntaxTree makeAndTree(const rsp::proto::ERDAbstractSyntaxTree& lhs,
                                              const rsp::proto::ERDAbstractSyntaxTree& rhs) {
    rsp::proto::ERDAbstractSyntaxTree tree;
    *tree.mutable_and_()->mutable_lhs() = lhs;
    *tree.mutable_and_()->mutable_rhs() = rhs;
    return tree;
}

rsp::proto::ERDAbstractSyntaxTree makeOrTree(const rsp::proto::ERDAbstractSyntaxTree& lhs,
                                             const rsp::proto::ERDAbstractSyntaxTree& rhs) {
    rsp::proto::ERDAbstractSyntaxTree tree;
    *tree.mutable_or_()->mutable_lhs() = lhs;
    *tree.mutable_or_()->mutable_rhs() = rhs;
    return tree;
}

rsp::proto::ERDAbstractSyntaxTree makeEqualsTree(const rsp::proto::ERDAbstractSyntaxTree& lhs,
                                                 const rsp::proto::ERDAbstractSyntaxTree& rhs) {
    rsp::proto::ERDAbstractSyntaxTree tree;
    *tree.mutable_equals()->mutable_lhs() = lhs;
    *tree.mutable_equals()->mutable_rhs() = rhs;
    return tree;
}

rsp::proto::ERDAbstractSyntaxTree makeAllOfTree(
    const std::vector<rsp::proto::ERDAbstractSyntaxTree>& terms) {
    rsp::proto::ERDAbstractSyntaxTree tree;
    for (const auto& term : terms) {
        *tree.mutable_all_of()->add_terms() = term;
    }
    return tree;
}

rsp::proto::ERDAbstractSyntaxTree makeAnyOfTree(
    const std::vector<rsp::proto::ERDAbstractSyntaxTree>& terms) {
    rsp::proto::ERDAbstractSyntaxTree tree;
    for (const auto& term : terms) {
        *tree.mutable_any_of()->add_terms() = term;
    }
    return tree;
}

rsp::proto::EndorsementNeeded makeRequirement(const rsp::proto::ERDAbstractSyntaxTree& tree) {
    rsp::proto::EndorsementNeeded requirement;
    *requirement.mutable_tree() = tree;
    return requirement;
}

bool isEmptyTree(const rsp::proto::ERDAbstractSyntaxTree& tree) {
    return tree.node_type_case() == rsp::proto::ERDAbstractSyntaxTree::NODE_TYPE_NOT_SET;
}

rsp::proto::NodeId toProtoMessageNodeId(const rsp::NodeID& nodeId) {
    rsp::proto::NodeId protoNodeId;
    std::string value(16, '\0');
    const uint64_t high = nodeId.high();
    const uint64_t low = nodeId.low();
    std::memcpy(value.data(), &high, sizeof(high));
    std::memcpy(value.data() + sizeof(high), &low, sizeof(low));
    protoNodeId.set_value(value);
    return protoNodeId;
}

rsp::proto::RSPMessage makeMessageWithDestination(const rsp::NodeID& destination) {
    rsp::proto::RSPMessage message;
    *message.mutable_destination() = toProtoMessageNodeId(destination);
    message.mutable_ping_request();
    return message;
}

rsp::proto::RSPMessage makeMessageWithSignatureSigner(const rsp::NodeID& source) {
    rsp::proto::RSPMessage message;
    *message.mutable_signature()->mutable_signer() = toProtoNodeId(source);
    message.mutable_ping_request();
    return message;
}

rsp::Endorsement makeTestEndorsement(const rsp::KeyPair& endorsementServiceKey,
                                     const rsp::KeyPair& subjectKey,
                                     const rsp::GUID& endorsementType,
                                     const std::string& endorsementValue) {
    return rsp::Endorsement::createSigned(
        endorsementServiceKey,
        subjectKey.nodeID(),
        endorsementType,
        rsp::Buffer(reinterpret_cast<const uint8_t*>(endorsementValue.data()),
                    static_cast<uint32_t>(endorsementValue.size())),
        rsp::DateTime::fromMillisecondsSinceEpoch(1712083200123ULL));
}

void testSerializationRoundTrip() {
    rsp::KeyPair endorsementServiceKey = rsp::KeyPair::generateP256();
    rsp::KeyPair subjectKey = rsp::KeyPair::generateP256();

    const uint8_t valueBytes[] = {0x10, 0x20, 0x30, 0x40};
    const rsp::Buffer endorsementValue(valueBytes, 4);
    const rsp::GUID endorsementType("00112233-4455-6677-8899-aabbccddeeff");
    const rsp::DateTime validUntil = rsp::DateTime::fromMillisecondsSinceEpoch(1712083200123ULL);

    const rsp::Endorsement endorsement = rsp::Endorsement::createSigned(
        endorsementServiceKey, subjectKey.nodeID(), endorsementType, endorsementValue, validUntil);

    require(endorsement.subject() == subjectKey.nodeID(), "endorsement should preserve subject");
    require(endorsement.endorsementService() == endorsementServiceKey.nodeID(),
            "endorsement should preserve endorsement service");
    require(endorsement.endorsementType() == endorsementType, "endorsement should preserve type");
    require(endorsement.endorsementValue().size() == 4, "endorsement should preserve value bytes");
    require(endorsement.validUntil().millisecondsSinceEpoch() == validUntil.millisecondsSinceEpoch(),
            "endorsement should preserve expiration");
    require(!endorsement.signature().empty(), "endorsement should contain a signature");

    const rsp::Buffer serialized = endorsement.serialize();
    require(!serialized.empty(), "serialized endorsement should not be empty");

    const rsp::Endorsement reparsed = rsp::Endorsement::deserialize(serialized);
    require(reparsed.subject() == endorsement.subject(), "deserialization should preserve subject");
    require(reparsed.endorsementService() == endorsement.endorsementService(),
            "deserialization should preserve endorsement service");
    require(reparsed.endorsementType() == endorsement.endorsementType(),
            "deserialization should preserve endorsement type");
    require(reparsed.endorsementValue().size() == endorsement.endorsementValue().size(),
            "deserialization should preserve endorsement value size");
    require(reparsed.validUntil().millisecondsSinceEpoch() == endorsement.validUntil().millisecondsSinceEpoch(),
            "deserialization should preserve expiration");
}

void testSignatureVerification() {
    rsp::KeyPair endorsementServiceKey = rsp::KeyPair::generateP256();
    rsp::KeyPair otherKey = rsp::KeyPair::generateP256();
    rsp::KeyPair subjectKey = rsp::KeyPair::generateP256();

    const uint8_t valueBytes[] = {0xAB, 0xCD, 0xEF};
    const rsp::Buffer endorsementValue(valueBytes, 3);
    const rsp::GUID endorsementType("12345678-90ab-cdef-1234-567890abcdef");
    const rsp::DateTime validUntil = rsp::DateTime::fromMillisecondsSinceEpoch(1800000000456ULL);

    const rsp::Endorsement endorsement = rsp::Endorsement::createSigned(
        endorsementServiceKey, subjectKey.nodeID(), endorsementType, endorsementValue, validUntil);

    require(endorsement.verifySignature(endorsementServiceKey),
            "endorsement should verify with the issuing key");
    require(!endorsement.verifySignature(otherKey),
            "endorsement should not verify with a different key");
}

void testTamperingInvalidatesSignature() {
    rsp::KeyPair endorsementServiceKey = rsp::KeyPair::generateP256();
    rsp::KeyPair subjectKey = rsp::KeyPair::generateP256();

    const uint8_t valueBytes[] = {0x01, 0x02, 0x03, 0x04, 0x05};
    const rsp::Buffer endorsementValue(valueBytes, 5);
    const rsp::GUID endorsementType("fedcba98-7654-3210-fedc-ba9876543210");
    const rsp::DateTime validUntil = rsp::DateTime::fromMillisecondsSinceEpoch(1900000000789ULL);

    const rsp::Endorsement endorsement = rsp::Endorsement::createSigned(
        endorsementServiceKey, subjectKey.nodeID(), endorsementType, endorsementValue, validUntil);

    rsp::Buffer tamperedSerialized = endorsement.serialize();
    tamperedSerialized.data()[tamperedSerialized.size() - 1] ^= 0x01;

    const rsp::Endorsement tampered = rsp::Endorsement::deserialize(tamperedSerialized);
    require(!tampered.verifySignature(endorsementServiceKey),
            "tampering with the serialized payload should invalidate the signature");
}

void testMalformedBufferRejection() {
    rsp::Buffer malformed(2);
    malformed.data()[0] = 0x00;
    malformed.data()[1] = 0x00;

    bool invalidThrown = false;
    try {
        static_cast<void>(rsp::Endorsement::deserialize(malformed));
    } catch (const std::runtime_error&) {
        invalidThrown = true;
    }

    require(invalidThrown, "deserializing a malformed endorsement should throw");
}

    void testRequirementTypeEquals() {
        rsp::KeyPair endorsementServiceKey = rsp::KeyPair::generateP256();
        rsp::KeyPair subjectKey = rsp::KeyPair::generateP256();
        const rsp::GUID matchingType("00112233-4455-6677-8899-aabbccddeeff");
        const rsp::GUID otherType("ffeeddcc-bbaa-9988-7766-554433221100");
        const rsp::Endorsement endorsement = makeTestEndorsement(endorsementServiceKey, subjectKey, matchingType, "alpha");

        require(rsp::endorsementMatchesRequirement(makeRequirement(makeEndorsementTypeEqualsTree(matchingType)), endorsement),
            "type-equals should match endorsements with the same type");
        require(!rsp::endorsementMatchesRequirement(makeRequirement(makeEndorsementTypeEqualsTree(otherType)), endorsement),
            "type-equals should reject endorsements with a different type");
    }

    void testRequirementValueEquals() {
        rsp::KeyPair endorsementServiceKey = rsp::KeyPair::generateP256();
        rsp::KeyPair subjectKey = rsp::KeyPair::generateP256();
        const rsp::GUID endorsementType("00112233-4455-6677-8899-aabbccddeeff");
        const rsp::Endorsement endorsement = makeTestEndorsement(endorsementServiceKey, subjectKey, endorsementType, "network-access");

        require(rsp::endorsementMatchesRequirement(makeRequirement(makeEndorsementValueEqualsTree("network-access")), endorsement),
            "value-equals should match endorsements with the same value bytes");
        require(!rsp::endorsementMatchesRequirement(makeRequirement(makeEndorsementValueEqualsTree("other-value")), endorsement),
            "value-equals should reject endorsements with a different value");
    }

    void testRequirementEndorsementSignerEquals() {
        rsp::KeyPair endorsementServiceKey = rsp::KeyPair::generateP256();
        rsp::KeyPair otherSignerKey = rsp::KeyPair::generateP256();
        rsp::KeyPair subjectKey = rsp::KeyPair::generateP256();
        const rsp::GUID endorsementType("00112233-4455-6677-8899-aabbccddeeff");
        const rsp::Endorsement endorsement = makeTestEndorsement(endorsementServiceKey, subjectKey, endorsementType, "alpha");

        require(rsp::endorsementMatchesRequirement(makeRequirement(makeEndorsementSignerEqualsTree(endorsementServiceKey.nodeID())), endorsement),
            "endorsement-signer-equals should match the endorsement signer");
        require(!rsp::endorsementMatchesRequirement(makeRequirement(makeEndorsementSignerEqualsTree(otherSignerKey.nodeID())), endorsement),
            "endorsement-signer-equals should reject a different signer");
        require(!rsp::endorsementMatchesRequirement(makeRequirement(makeMessageDestinationTree(subjectKey.nodeID())), endorsement),
            "message-destination should not match an endorsement without message context");
        require(!rsp::endorsementMatchesRequirement(makeRequirement(makeMessageSourceTree(subjectKey.nodeID())), endorsement),
            "message-source should not match an endorsement without message context");
    }

    void testRequirementAndOrEqualsNodes() {
        rsp::KeyPair endorsementServiceKey = rsp::KeyPair::generateP256();
        rsp::KeyPair otherSignerKey = rsp::KeyPair::generateP256();
        rsp::KeyPair subjectKey = rsp::KeyPair::generateP256();
        const rsp::GUID matchingType("00112233-4455-6677-8899-aabbccddeeff");
        const rsp::GUID otherType("ffeeddcc-bbaa-9988-7766-554433221100");
        const rsp::Endorsement endorsement = makeTestEndorsement(endorsementServiceKey, subjectKey, matchingType, "network-access");

        const auto typeMatches = makeEndorsementTypeEqualsTree(matchingType);
        const auto typeMisses = makeEndorsementTypeEqualsTree(otherType);
        const auto valueMatches = makeEndorsementValueEqualsTree("network-access");
        const auto signerMatches = makeEndorsementSignerEqualsTree(endorsementServiceKey.nodeID());
        const auto signerMisses = makeEndorsementSignerEqualsTree(otherSignerKey.nodeID());

        require(rsp::endorsementMatchesRequirement(makeRequirement(makeAndTree(typeMatches, valueMatches)), endorsement),
            "and should match when both operands match");
        require(!rsp::endorsementMatchesRequirement(makeRequirement(makeAndTree(typeMatches, signerMisses)), endorsement),
            "and should fail when either operand fails");
        require(rsp::endorsementMatchesRequirement(makeRequirement(makeOrTree(typeMisses, signerMatches)), endorsement),
            "or should match when one operand matches");
        require(!rsp::endorsementMatchesRequirement(makeRequirement(makeOrTree(typeMisses, signerMisses)), endorsement),
            "or should fail when both operands fail");
        require(rsp::endorsementMatchesRequirement(makeRequirement(makeEqualsTree(typeMatches, signerMatches)), endorsement),
            "equals should match when both operands evaluate to the same boolean");
        require(!rsp::endorsementMatchesRequirement(makeRequirement(makeEqualsTree(typeMatches, signerMisses)), endorsement),
            "equals should fail when operands evaluate to different booleans");
    }

    void testRequirementComplexAst() {
        rsp::KeyPair endorsementServiceKey = rsp::KeyPair::generateP256();
        rsp::KeyPair otherSignerKey = rsp::KeyPair::generateP256();
        rsp::KeyPair subjectKey = rsp::KeyPair::generateP256();
        const rsp::GUID matchingType("00112233-4455-6677-8899-aabbccddeeff");
        const rsp::GUID otherType("ffeeddcc-bbaa-9988-7766-554433221100");
        const rsp::Endorsement endorsement = makeTestEndorsement(endorsementServiceKey, subjectKey, matchingType, "network-access");

        const auto leftBranch = makeAndTree(makeEndorsementTypeEqualsTree(matchingType), makeEndorsementValueEqualsTree("network-access"));
        const auto rightBranch = makeAndTree(makeEndorsementSignerEqualsTree(otherSignerKey.nodeID()), makeEndorsementTypeEqualsTree(otherType));
        const auto complexTree = makeOrTree(leftBranch, rightBranch);

        require(rsp::endorsementMatchesRequirement(makeRequirement(complexTree), endorsement),
            "complex AST should match when one nested branch matches");

        rsp::proto::EndorsementNeeded emptyRequirement;
        require(!rsp::endorsementMatchesRequirement(emptyRequirement, endorsement),
            "requirements without a tree should not match");
    }

    void testRequirementTrueFalseNodes() {
        rsp::KeyPair endorsementServiceKey = rsp::KeyPair::generateP256();
        rsp::KeyPair subjectKey = rsp::KeyPair::generateP256();
        const rsp::GUID endorsementType("00112233-4455-6677-8899-aabbccddeeff");
        const rsp::Endorsement endorsement = makeTestEndorsement(endorsementServiceKey, subjectKey, endorsementType, "network-access");

        require(rsp::endorsementMatchesRequirement(makeRequirement(makeTrueTree()), endorsement),
            "true nodes should always evaluate to true");
        require(!rsp::endorsementMatchesRequirement(makeRequirement(makeFalseTree()), endorsement),
            "false nodes should always evaluate to false");
        require(rsp::endorsementMatchesRequirement(makeRequirement(makeEqualsTree(makeFalseTree(), makeFalseTree())), endorsement),
            "equals should treat false nodes as ordinary boolean operands");
    }

    void testRequirementAllOfAnyOfNodes() {
        rsp::KeyPair endorsementServiceKey = rsp::KeyPair::generateP256();
        rsp::KeyPair otherSignerKey = rsp::KeyPair::generateP256();
        rsp::KeyPair subjectKey = rsp::KeyPair::generateP256();
        const rsp::GUID matchingType("00112233-4455-6677-8899-aabbccddeeff");
        const rsp::GUID otherType("ffeeddcc-bbaa-9988-7766-554433221100");
        const rsp::Endorsement endorsement = makeTestEndorsement(endorsementServiceKey, subjectKey, matchingType, "network-access");

        const auto allOfMatches = makeAllOfTree({
            makeEndorsementTypeEqualsTree(matchingType),
            makeEndorsementValueEqualsTree("network-access"),
            makeTrueTree(),
        });
        require(rsp::endorsementMatchesRequirement(makeRequirement(allOfMatches), endorsement),
            "all-of should match when all terms match");

        const auto allOfMisses = makeAllOfTree({
            makeEndorsementTypeEqualsTree(matchingType),
            makeEndorsementSignerEqualsTree(otherSignerKey.nodeID()),
        });
        require(!rsp::endorsementMatchesRequirement(makeRequirement(allOfMisses), endorsement),
            "all-of should fail when any term fails");

        const auto anyOfMatches = makeAnyOfTree({
            makeEndorsementTypeEqualsTree(otherType),
            makeEndorsementSignerEqualsTree(otherSignerKey.nodeID()),
            makeEndorsementValueEqualsTree("network-access"),
        });
        require(rsp::endorsementMatchesRequirement(makeRequirement(anyOfMatches), endorsement),
            "any-of should match when any term matches");

        const auto anyOfMisses = makeAnyOfTree({
            makeEndorsementTypeEqualsTree(otherType),
            makeEndorsementSignerEqualsTree(otherSignerKey.nodeID()),
            makeFalseTree(),
        });
        require(!rsp::endorsementMatchesRequirement(makeRequirement(anyOfMisses), endorsement),
            "any-of should fail when all terms fail");
    }

    void testReduceRequirementTreeEliminatesMatchedAndBranch() {
        rsp::KeyPair endorsementServiceKey = rsp::KeyPair::generateP256();
        rsp::KeyPair subjectKey = rsp::KeyPair::generateP256();
        const rsp::GUID matchingType("00112233-4455-6677-8899-aabbccddeeff");
        const rsp::Endorsement endorsement = makeTestEndorsement(endorsementServiceKey, subjectKey, matchingType, "network-access");

        const auto tree = makeAndTree(makeEndorsementTypeEqualsTree(matchingType), makeEndorsementValueEqualsTree("other-value"));
        const auto reduced = rsp::reduceRequirementTree(tree, std::vector<rsp::Endorsement>{endorsement});

        require(reduced.node_type_case() == rsp::proto::ERDAbstractSyntaxTree::kEndorsementValueEquals,
            "reduction should keep only the unmatched value predicate from an and-tree");
        require(reduced.endorsement_value_equals().value() == "other-value",
            "reduction should preserve the unmatched value predicate content");
    }

    void testReduceRequirementTreeEliminatesSatisfiedOrTree() {
        rsp::KeyPair endorsementServiceKey = rsp::KeyPair::generateP256();
        rsp::KeyPair subjectKey = rsp::KeyPair::generateP256();
        const rsp::GUID matchingType("00112233-4455-6677-8899-aabbccddeeff");
        const rsp::GUID otherType("ffeeddcc-bbaa-9988-7766-554433221100");
        const rsp::Endorsement endorsement = makeTestEndorsement(endorsementServiceKey, subjectKey, matchingType, "network-access");

        const auto tree = makeOrTree(makeEndorsementTypeEqualsTree(matchingType), makeEndorsementTypeEqualsTree(otherType));
        const auto reduced = rsp::reduceRequirementTree(tree, std::vector<rsp::Endorsement>{endorsement});

        require(isEmptyTree(reduced),
            "reduction should clear an or-tree once any branch is already satisfied");
    }

    void testReduceRequirementTreePreservesUnmetAlternatives() {
        rsp::KeyPair endorsementServiceKey = rsp::KeyPair::generateP256();
        rsp::KeyPair subjectKey = rsp::KeyPair::generateP256();
        const rsp::GUID matchingType("00112233-4455-6677-8899-aabbccddeeff");
        const rsp::GUID otherType("ffeeddcc-bbaa-9988-7766-554433221100");
        const rsp::Endorsement endorsement = makeTestEndorsement(endorsementServiceKey, subjectKey, matchingType, "network-access");

        const auto leftBranch = makeAndTree(makeEndorsementTypeEqualsTree(matchingType), makeEndorsementValueEqualsTree("missing-value"));
        const auto rightBranch = makeEndorsementSignerEqualsTree(rsp::KeyPair::generateP256().nodeID());
        const auto tree = makeOrTree(leftBranch, rightBranch);
        const auto reduced = rsp::reduceRequirementTree(tree, std::vector<rsp::Endorsement>{endorsement});

        require(reduced.node_type_case() == rsp::proto::ERDAbstractSyntaxTree::kOr,
            "reduction should keep unmet alternatives in an or-tree");
        require(reduced.or_().lhs().node_type_case() == rsp::proto::ERDAbstractSyntaxTree::kEndorsementValueEquals,
            "reduction should drop the matched portion of the partially satisfied branch");
        require(reduced.or_().rhs().node_type_case() == rsp::proto::ERDAbstractSyntaxTree::kEndorsementSignerEquals,
            "reduction should preserve the untouched alternative branch");
    }

    void testReduceRequirementTreeClearsFullySatisfiedTree() {
        rsp::KeyPair endorsementServiceKey = rsp::KeyPair::generateP256();
        rsp::KeyPair subjectKey = rsp::KeyPair::generateP256();
        const rsp::GUID matchingType("00112233-4455-6677-8899-aabbccddeeff");
        const rsp::Endorsement endorsement = makeTestEndorsement(endorsementServiceKey, subjectKey, matchingType, "network-access");

        const auto tree = makeAndTree(makeEndorsementTypeEqualsTree(matchingType), makeEndorsementValueEqualsTree("network-access"));
        const auto reduced = rsp::reduceRequirementTree(tree, std::vector<rsp::Endorsement>{endorsement});

        require(isEmptyTree(reduced),
            "reduction should clear a fully satisfied tree");
    }

    void testReduceRequirementTreeSimplifiesTrueAndFalseConstants() {
        const auto reducedTrue = rsp::reduceRequirementTree(makeTrueTree(), std::vector<rsp::Endorsement>{});
        require(isEmptyTree(reducedTrue),
            "reduction should eliminate true because it is already satisfied");

        const auto reducedFalse = rsp::reduceRequirementTree(makeFalseTree(), std::vector<rsp::Endorsement>{});
        require(reducedFalse.node_type_case() == rsp::proto::ERDAbstractSyntaxTree::kFalseValue,
            "reduction should preserve false because it can never be satisfied");

        const auto reducedAnd = rsp::reduceRequirementTree(
            makeAndTree(makeTrueTree(), makeEndorsementValueEqualsTree("missing-value")),
            std::vector<rsp::Endorsement>{});
        require(reducedAnd.node_type_case() == rsp::proto::ERDAbstractSyntaxTree::kEndorsementValueEquals,
            "true and X should reduce to X");

        const auto reducedOr = rsp::reduceRequirementTree(
            makeOrTree(makeFalseTree(), makeEndorsementValueEqualsTree("missing-value")),
            std::vector<rsp::Endorsement>{});
        require(reducedOr.node_type_case() == rsp::proto::ERDAbstractSyntaxTree::kEndorsementValueEquals,
            "false or X should reduce to X");
    }

    void testReduceRequirementTreeSimplifiesAllOfAnyOf() {
        rsp::KeyPair endorsementServiceKey = rsp::KeyPair::generateP256();
        rsp::KeyPair subjectKey = rsp::KeyPair::generateP256();
        const rsp::GUID matchingType("00112233-4455-6677-8899-aabbccddeeff");
        const rsp::Endorsement endorsement = makeTestEndorsement(endorsementServiceKey, subjectKey, matchingType, "network-access");

        const auto reducedAllOf = rsp::reduceRequirementTree(
            makeAllOfTree({
                makeEndorsementTypeEqualsTree(matchingType),
                makeTrueTree(),
                makeEndorsementValueEqualsTree("missing-value"),
            }),
            std::vector<rsp::Endorsement>{endorsement});
        require(reducedAllOf.node_type_case() == rsp::proto::ERDAbstractSyntaxTree::kEndorsementValueEquals,
            "all-of should reduce by dropping satisfied terms");

        const auto reducedAnyOf = rsp::reduceRequirementTree(
            makeAnyOfTree({
                makeFalseTree(),
                makeEndorsementTypeEqualsTree(matchingType),
                makeEndorsementValueEqualsTree("missing-value"),
            }),
            std::vector<rsp::Endorsement>{endorsement});
        require(isEmptyTree(reducedAnyOf),
            "any-of should reduce to empty once any term is already satisfied");

        const auto reducedAnyOfFalse = rsp::reduceRequirementTree(
            makeAnyOfTree({makeFalseTree(), makeFalseTree()}),
            std::vector<rsp::Endorsement>{});
        require(reducedAnyOfFalse.node_type_case() == rsp::proto::ERDAbstractSyntaxTree::kFalseValue,
            "any-of with only impossible terms should reduce to false");
    }

    void testReduceRequirementTreePreservesMessageLogicWithoutMessageContext() {
        const rsp::NodeID expectedDestination = rsp::KeyPair::generateP256().nodeID();
        const auto tree = makeAndTree(makeMessageDestinationTree(expectedDestination), makeTrueTree());

        const auto reduced = rsp::reduceRequirementTree(tree, std::vector<rsp::Endorsement>{});

        require(reduced.node_type_case() == rsp::proto::ERDAbstractSyntaxTree::kMessageDestination,
            "reduction without a message should preserve message predicates");
        require(reduced.message_destination().destination().value() == toProtoNodeId(expectedDestination).value(),
            "reduction without a message should preserve the original destination bytes");
    }

    void testReduceRequirementTreeEliminatesMismatchedMessageBranches() {
        const rsp::NodeID expectedDestination = rsp::KeyPair::generateP256().nodeID();
        const rsp::NodeID actualDestination = rsp::KeyPair::generateP256().nodeID();
        const auto tree = makeOrTree(makeMessageDestinationTree(expectedDestination), makeEndorsementValueEqualsTree("network-access"));
        const auto message = makeMessageWithDestination(actualDestination);

        const auto reduced = rsp::reduceRequirementTree(tree, std::vector<rsp::Endorsement>{}, &message);

        require(reduced.node_type_case() == rsp::proto::ERDAbstractSyntaxTree::kEndorsementValueEquals,
            "reduction with a non-matching message should prune the mismatched message branch");
        require(reduced.endorsement_value_equals().value() == "network-access",
            "reduction should preserve the remaining unmatched branch");
    }

    void testReduceRequirementTreeEliminatesMatchedMessageSource() {
        const rsp::NodeID expectedSource = rsp::KeyPair::generateP256().nodeID();
        const auto tree = makeAndTree(makeMessageSourceTree(expectedSource), makeTrueTree());
        const auto message = makeMessageWithSignatureSigner(expectedSource);

        const auto reduced = rsp::reduceRequirementTree(tree, std::vector<rsp::Endorsement>{}, &message);

        require(isEmptyTree(reduced),
            "reduction with a matching signature signer should eliminate the message-source predicate");
    }

}  // namespace

int main() {
    try {
        testSerializationRoundTrip();
        testSignatureVerification();
        testTamperingInvalidatesSignature();
        testMalformedBufferRejection();
        testRequirementTypeEquals();
        testRequirementValueEquals();
        testRequirementEndorsementSignerEquals();
        testRequirementAndOrEqualsNodes();
        testRequirementComplexAst();
        testRequirementTrueFalseNodes();
        testRequirementAllOfAnyOfNodes();
        testReduceRequirementTreeEliminatesMatchedAndBranch();
        testReduceRequirementTreeEliminatesSatisfiedOrTree();
        testReduceRequirementTreePreservesUnmetAlternatives();
        testReduceRequirementTreeClearsFullySatisfiedTree();
        testReduceRequirementTreeSimplifiesTrueAndFalseConstants();
        testReduceRequirementTreeSimplifiesAllOfAnyOf();
        testReduceRequirementTreePreservesMessageLogicWithoutMessageContext();
        testReduceRequirementTreeEliminatesMismatchedMessageBranches();
        testReduceRequirementTreeEliminatesMatchedMessageSource();

        std::cout << "endorsement_test passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "endorsement_test failed: " << exception.what() << '\n';
        return 1;
    }
}