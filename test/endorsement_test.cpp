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
    *tree.mutable_endorsement()->mutable_tree()->mutable_type_equals()->mutable_type() = toProtoEndorsementType(endorsementType);
    return tree;
}

rsp::proto::ERDAbstractSyntaxTree makeEndorsementValueEqualsTree(const std::string& value) {
    rsp::proto::ERDAbstractSyntaxTree tree;
    tree.mutable_endorsement()->mutable_tree()->mutable_value_equals()->set_value(value);
    return tree;
}

rsp::proto::ERDAbstractSyntaxTree makeEndorsementSignerEqualsTree(const rsp::NodeID& signer) {
    rsp::proto::ERDAbstractSyntaxTree tree;
    *tree.mutable_endorsement()->mutable_tree()->mutable_signer_equals()->mutable_signer() = toProtoNodeId(signer);
    return tree;
}

rsp::proto::ERDAbstractSyntaxTree makeMessageDestinationTree(const rsp::NodeID& destination) {
    rsp::proto::ERDAbstractSyntaxTree tree;
    *tree.mutable_message()->mutable_tree()->mutable_destination()->mutable_destination() = toProtoNodeId(destination);
    return tree;
}

rsp::proto::ERDAbstractSyntaxTree makeMessageSourceTree(const rsp::NodeID& source) {
    rsp::proto::ERDAbstractSyntaxTree tree;
    *tree.mutable_message()->mutable_tree()->mutable_source()->mutable_source() = toProtoNodeId(source);
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
    require(serialized.size() >= 4, "serialized endorsement should include the custom format header");
    require(serialized.data()[0] == static_cast<uint8_t>('R') &&
                serialized.data()[1] == static_cast<uint8_t>('S') &&
                serialized.data()[2] == static_cast<uint8_t>('E') &&
                serialized.data()[3] == static_cast<uint8_t>('1'),
        "serialized endorsement should use the RSE1 disk format header");

    rsp::proto::Endorsement protobufMessage;
    require(!protobufMessage.ParseFromArray(serialized.data(), static_cast<int>(serialized.size())),
        "serialized endorsement should no longer be a protobuf payload");

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

    void testProtoRoundTrip() {
        rsp::KeyPair endorsementServiceKey = rsp::KeyPair::generateP256();
        rsp::KeyPair subjectKey = rsp::KeyPair::generateP256();

        const uint8_t valueBytes[] = {0xDE, 0xAD, 0xBE, 0xEF};
        const rsp::Buffer endorsementValue(valueBytes, 4);
        const rsp::GUID endorsementType("12345678-90ab-cdef-1234-567890abcdef");
        const rsp::DateTime validUntil = rsp::DateTime::fromMillisecondsSinceEpoch(1800000000456ULL);

        const rsp::Endorsement endorsement = rsp::Endorsement::createSigned(
        endorsementServiceKey, subjectKey.nodeID(), endorsementType, endorsementValue, validUntil);

        const rsp::proto::Endorsement protoMessage = endorsement.toProto();
        const rsp::Endorsement reparsed = rsp::Endorsement::fromProto(protoMessage);

        require(reparsed.subject() == endorsement.subject(), "proto conversion should preserve subject");
        require(reparsed.endorsementService() == endorsement.endorsementService(),
            "proto conversion should preserve endorsement service");
        require(reparsed.endorsementType() == endorsement.endorsementType(),
            "proto conversion should preserve endorsement type");
        require(reparsed.validUntil().millisecondsSinceEpoch() == endorsement.validUntil().millisecondsSinceEpoch(),
            "proto conversion should preserve expiration");
        require(reparsed.verifySignature(endorsementServiceKey),
            "proto conversion should preserve a valid signature");
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

        require(reduced.node_type_case() == rsp::proto::ERDAbstractSyntaxTree::kEndorsement,
            "reduction should keep only the unmatched value predicate from an and-tree");
        require(reduced.endorsement().tree().value_equals().value() == "other-value",
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
        require(reduced.or_().lhs().node_type_case() == rsp::proto::ERDAbstractSyntaxTree::kEndorsement,
            "reduction should drop the matched portion of the partially satisfied branch");
        require(reduced.or_().rhs().node_type_case() == rsp::proto::ERDAbstractSyntaxTree::kEndorsement,
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
        require(reducedAnd.node_type_case() == rsp::proto::ERDAbstractSyntaxTree::kEndorsement,
            "true and X should reduce to X");

        const auto reducedOr = rsp::reduceRequirementTree(
            makeOrTree(makeFalseTree(), makeEndorsementValueEqualsTree("missing-value")),
            std::vector<rsp::Endorsement>{});
        require(reducedOr.node_type_case() == rsp::proto::ERDAbstractSyntaxTree::kEndorsement,
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
        require(reducedAllOf.node_type_case() == rsp::proto::ERDAbstractSyntaxTree::kEndorsement,
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

        require(reduced.node_type_case() == rsp::proto::ERDAbstractSyntaxTree::kMessage,
            "reduction without a message should preserve message predicates");
        require(reduced.message().tree().destination().destination().value() == toProtoNodeId(expectedDestination).value(),
            "reduction without a message should preserve the original destination bytes");
    }

    void testReduceRequirementTreeEliminatesMismatchedMessageBranches() {
        const rsp::NodeID expectedDestination = rsp::KeyPair::generateP256().nodeID();
        const rsp::NodeID actualDestination = rsp::KeyPair::generateP256().nodeID();
        const auto tree = makeOrTree(makeMessageDestinationTree(expectedDestination), makeEndorsementValueEqualsTree("network-access"));
        const auto message = makeMessageWithDestination(actualDestination);

        const auto reduced = rsp::reduceRequirementTree(tree, std::vector<rsp::Endorsement>{}, &message);

        require(reduced.node_type_case() == rsp::proto::ERDAbstractSyntaxTree::kEndorsement,
            "reduction with a non-matching message should prune the mismatched message branch");
        require(reduced.endorsement().tree().value_equals().value() == "network-access",
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

    // Helper: builds an ERDAbstractSyntaxTree with a single ERDASTEndorsement wrapping
    // an AND of type_equals and value_equals inside the endorsement sub-tree.
    // This ensures both checks are applied to the SAME endorsement.
    rsp::proto::ERDAbstractSyntaxTree makeEndorsementTypeAndValueTree(
        const rsp::GUID& endorsementType, const std::string& value) {
        rsp::proto::ERDAbstractSyntaxTree tree;
        auto* endTree = tree.mutable_endorsement()->mutable_tree();
        auto* andNode = endTree->mutable_and_();
        *andNode->mutable_lhs()->mutable_type_equals()->mutable_type() = toProtoEndorsementType(endorsementType);
        andNode->mutable_rhs()->mutable_value_equals()->set_value(value);
        return tree;
    }

    void testCrossEndorsementMatchingPrevented() {
        // Two endorsements with different type/value combinations:
        //   endorsement A: type=typeA, value="alpha"
        //   endorsement B: type=typeB, value="beta"
        // A requirement asking for type=typeA AND value="beta" must NOT match,
        // because no single endorsement has that combination.
        rsp::KeyPair esKey = rsp::KeyPair::generateP256();
        rsp::KeyPair subjectKey = rsp::KeyPair::generateP256();
        const rsp::GUID typeA("00112233-4455-6677-8899-aabbccddeeff");
        const rsp::GUID typeB("ffeeddcc-bbaa-9988-7766-554433221100");
        const rsp::Endorsement endorsementA = makeTestEndorsement(esKey, subjectKey, typeA, "alpha");
        const rsp::Endorsement endorsementB = makeTestEndorsement(esKey, subjectKey, typeB, "beta");

        // Requirement: single endorsement must have typeA AND value "beta"
        const auto crossReq = makeEndorsementTypeAndValueTree(typeA, "beta");

        // Neither endorsement alone satisfies both predicates
        require(!rsp::endorsementMatchesRequirement(makeRequirement(crossReq), endorsementA),
            "endorsement A (typeA/alpha) should not match typeA+beta");
        require(!rsp::endorsementMatchesRequirement(makeRequirement(crossReq), endorsementB),
            "endorsement B (typeB/beta) should not match typeA+beta");

        // Correct combinations DO match
        const auto correctReqA = makeEndorsementTypeAndValueTree(typeA, "alpha");
        require(rsp::endorsementMatchesRequirement(makeRequirement(correctReqA), endorsementA),
            "endorsement A should match typeA+alpha");

        const auto correctReqB = makeEndorsementTypeAndValueTree(typeB, "beta");
        require(rsp::endorsementMatchesRequirement(makeRequirement(correctReqB), endorsementB),
            "endorsement B should match typeB+beta");
    }

    void testReduceWithMultipleEndorsementsNoCrossMatching() {
        // Verify that reduceRequirementTree with multiple endorsements does NOT
        // satisfy a requirement by mixing attributes across endorsements.
        rsp::KeyPair esKey = rsp::KeyPair::generateP256();
        rsp::KeyPair subjectKey = rsp::KeyPair::generateP256();
        const rsp::GUID typeA("00112233-4455-6677-8899-aabbccddeeff");
        const rsp::GUID typeB("ffeeddcc-bbaa-9988-7766-554433221100");
        const rsp::Endorsement endorsementA = makeTestEndorsement(esKey, subjectKey, typeA, "alpha");
        const rsp::Endorsement endorsementB = makeTestEndorsement(esKey, subjectKey, typeB, "beta");
        const std::vector<rsp::Endorsement> endorsements = {endorsementA, endorsementB};

        // Cross-matching requirement: typeA + value "beta" — no single endorsement has this
        const auto crossReq = makeEndorsementTypeAndValueTree(typeA, "beta");
        const auto reduced = rsp::reduceRequirementTree(crossReq, endorsements);

        // The tree should NOT be fully reduced (not empty) because no single endorsement matches
        require(!isEmptyTree(reduced),
            "cross-matched type/value across endorsements should not reduce to empty");

        // But a correctly matching requirement SHOULD reduce to empty
        const auto correctReq = makeEndorsementTypeAndValueTree(typeA, "alpha");
        const auto reducedCorrect = rsp::reduceRequirementTree(correctReq, endorsements);
        require(isEmptyTree(reducedCorrect),
            "correctly matched type+value on same endorsement should reduce to empty");
    }

    void testMultipleEndorsementsWithSignerCrossMatching() {
        // endorsement A: signerKey1, typeX
        // endorsement B: signerKey2, typeY
        // Requirement: signerKey1 AND typeY — must NOT match any single endorsement
        rsp::KeyPair signerKey1 = rsp::KeyPair::generateP256();
        rsp::KeyPair signerKey2 = rsp::KeyPair::generateP256();
        rsp::KeyPair subjectKey = rsp::KeyPair::generateP256();
        const rsp::GUID typeX("11111111-1111-1111-1111-111111111111");
        const rsp::GUID typeY("22222222-2222-2222-2222-222222222222");
        const rsp::Endorsement endorsementA = makeTestEndorsement(signerKey1, subjectKey, typeX, "val");
        const rsp::Endorsement endorsementB = makeTestEndorsement(signerKey2, subjectKey, typeY, "val");
        const std::vector<rsp::Endorsement> endorsements = {endorsementA, endorsementB};

        // Build: single endorsement must have signer=signerKey1 AND type=typeY
        rsp::proto::ERDAbstractSyntaxTree crossReq;
        {
            auto* endTree = crossReq.mutable_endorsement()->mutable_tree();
            auto* andNode = endTree->mutable_and_();
            *andNode->mutable_lhs()->mutable_signer_equals()->mutable_signer() = toProtoNodeId(signerKey1.nodeID());
            *andNode->mutable_rhs()->mutable_type_equals()->mutable_type() = toProtoEndorsementType(typeY);
        }

        const auto reduced = rsp::reduceRequirementTree(crossReq, endorsements);
        require(!isEmptyTree(reduced),
            "signer from endorsement A + type from endorsement B should not reduce to empty");

        // Correct: signerKey1 AND typeX — endorsement A has both
        rsp::proto::ERDAbstractSyntaxTree correctReq;
        {
            auto* endTree = correctReq.mutable_endorsement()->mutable_tree();
            auto* andNode = endTree->mutable_and_();
            *andNode->mutable_lhs()->mutable_signer_equals()->mutable_signer() = toProtoNodeId(signerKey1.nodeID());
            *andNode->mutable_rhs()->mutable_type_equals()->mutable_type() = toProtoEndorsementType(typeX);
        }

        const auto reducedCorrect = rsp::reduceRequirementTree(correctReq, endorsements);
        require(isEmptyTree(reducedCorrect),
            "signer and type from same endorsement should reduce to empty");
    }

}  // namespace

int main() {
    try {
        testSerializationRoundTrip();
        testSignatureVerification();
        testProtoRoundTrip();
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
        testCrossEndorsementMatchingPrevented();
        testReduceWithMultipleEndorsementsNoCrossMatching();
        testMultipleEndorsementsWithSignerCrossMatching();

        std::cout << "endorsement_test passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "endorsement_test failed: " << exception.what() << '\n';
        return 1;
    }
}