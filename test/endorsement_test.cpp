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

rsp::proto::ERDAbstractSyntaxTree makeTypeEqualsTree(const rsp::GUID& endorsementType) {
    rsp::proto::ERDAbstractSyntaxTree tree;
    *tree.mutable_type_equals()->mutable_type() = toProtoEndorsementType(endorsementType);
    return tree;
}

rsp::proto::ERDAbstractSyntaxTree makeValueEqualsTree(const std::string& value) {
    rsp::proto::ERDAbstractSyntaxTree tree;
    tree.mutable_value_equals()->set_value(value);
    return tree;
}

rsp::proto::ERDAbstractSyntaxTree makeSignerEqualsTree(const rsp::NodeID& signer) {
    rsp::proto::ERDAbstractSyntaxTree tree;
    *tree.mutable_signer_equals()->mutable_signer() = toProtoNodeId(signer);
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

rsp::proto::EndorsementNeeded makeRequirement(const rsp::proto::ERDAbstractSyntaxTree& tree) {
    rsp::proto::EndorsementNeeded requirement;
    *requirement.mutable_tree() = tree;
    return requirement;
}

bool isEmptyTree(const rsp::proto::ERDAbstractSyntaxTree& tree) {
    return tree.node_type_case() == rsp::proto::ERDAbstractSyntaxTree::NODE_TYPE_NOT_SET;
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

        require(rsp::endorsementMatchesRequirement(makeRequirement(makeTypeEqualsTree(matchingType)), endorsement),
            "type-equals should match endorsements with the same type");
        require(!rsp::endorsementMatchesRequirement(makeRequirement(makeTypeEqualsTree(otherType)), endorsement),
            "type-equals should reject endorsements with a different type");
    }

    void testRequirementValueEquals() {
        rsp::KeyPair endorsementServiceKey = rsp::KeyPair::generateP256();
        rsp::KeyPair subjectKey = rsp::KeyPair::generateP256();
        const rsp::GUID endorsementType("00112233-4455-6677-8899-aabbccddeeff");
        const rsp::Endorsement endorsement = makeTestEndorsement(endorsementServiceKey, subjectKey, endorsementType, "network-access");

        require(rsp::endorsementMatchesRequirement(makeRequirement(makeValueEqualsTree("network-access")), endorsement),
            "value-equals should match endorsements with the same value bytes");
        require(!rsp::endorsementMatchesRequirement(makeRequirement(makeValueEqualsTree("other-value")), endorsement),
            "value-equals should reject endorsements with a different value");
    }

    void testRequirementSignerEquals() {
        rsp::KeyPair endorsementServiceKey = rsp::KeyPair::generateP256();
        rsp::KeyPair otherSignerKey = rsp::KeyPair::generateP256();
        rsp::KeyPair subjectKey = rsp::KeyPair::generateP256();
        const rsp::GUID endorsementType("00112233-4455-6677-8899-aabbccddeeff");
        const rsp::Endorsement endorsement = makeTestEndorsement(endorsementServiceKey, subjectKey, endorsementType, "alpha");

        require(rsp::endorsementMatchesRequirement(makeRequirement(makeSignerEqualsTree(endorsementServiceKey.nodeID())), endorsement),
            "signer-equals should match the endorsement signer");
        require(!rsp::endorsementMatchesRequirement(makeRequirement(makeSignerEqualsTree(otherSignerKey.nodeID())), endorsement),
            "signer-equals should reject a different signer");
    }

    void testRequirementAndOrEqualsNodes() {
        rsp::KeyPair endorsementServiceKey = rsp::KeyPair::generateP256();
        rsp::KeyPair otherSignerKey = rsp::KeyPair::generateP256();
        rsp::KeyPair subjectKey = rsp::KeyPair::generateP256();
        const rsp::GUID matchingType("00112233-4455-6677-8899-aabbccddeeff");
        const rsp::GUID otherType("ffeeddcc-bbaa-9988-7766-554433221100");
        const rsp::Endorsement endorsement = makeTestEndorsement(endorsementServiceKey, subjectKey, matchingType, "network-access");

        const auto typeMatches = makeTypeEqualsTree(matchingType);
        const auto typeMisses = makeTypeEqualsTree(otherType);
        const auto valueMatches = makeValueEqualsTree("network-access");
        const auto signerMatches = makeSignerEqualsTree(endorsementServiceKey.nodeID());
        const auto signerMisses = makeSignerEqualsTree(otherSignerKey.nodeID());

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

        const auto leftBranch = makeAndTree(makeTypeEqualsTree(matchingType), makeValueEqualsTree("network-access"));
        const auto rightBranch = makeAndTree(makeSignerEqualsTree(otherSignerKey.nodeID()), makeTypeEqualsTree(otherType));
        const auto complexTree = makeOrTree(leftBranch, rightBranch);

        require(rsp::endorsementMatchesRequirement(makeRequirement(complexTree), endorsement),
            "complex AST should match when one nested branch matches");

        rsp::proto::EndorsementNeeded emptyRequirement;
        require(!rsp::endorsementMatchesRequirement(emptyRequirement, endorsement),
            "requirements without a tree should not match");
    }

    void testReduceRequirementTreeEliminatesMatchedAndBranch() {
        rsp::KeyPair endorsementServiceKey = rsp::KeyPair::generateP256();
        rsp::KeyPair subjectKey = rsp::KeyPair::generateP256();
        const rsp::GUID matchingType("00112233-4455-6677-8899-aabbccddeeff");
        const rsp::Endorsement endorsement = makeTestEndorsement(endorsementServiceKey, subjectKey, matchingType, "network-access");

        const auto tree = makeAndTree(makeTypeEqualsTree(matchingType), makeValueEqualsTree("other-value"));
        const auto reduced = rsp::reduceRequirementTree(tree, std::vector<rsp::Endorsement>{endorsement});

        require(reduced.node_type_case() == rsp::proto::ERDAbstractSyntaxTree::kValueEquals,
            "reduction should keep only the unmatched value predicate from an and-tree");
        require(reduced.value_equals().value() == "other-value",
            "reduction should preserve the unmatched value predicate content");
    }

    void testReduceRequirementTreeEliminatesSatisfiedOrTree() {
        rsp::KeyPair endorsementServiceKey = rsp::KeyPair::generateP256();
        rsp::KeyPair subjectKey = rsp::KeyPair::generateP256();
        const rsp::GUID matchingType("00112233-4455-6677-8899-aabbccddeeff");
        const rsp::GUID otherType("ffeeddcc-bbaa-9988-7766-554433221100");
        const rsp::Endorsement endorsement = makeTestEndorsement(endorsementServiceKey, subjectKey, matchingType, "network-access");

        const auto tree = makeOrTree(makeTypeEqualsTree(matchingType), makeTypeEqualsTree(otherType));
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

        const auto leftBranch = makeAndTree(makeTypeEqualsTree(matchingType), makeValueEqualsTree("missing-value"));
        const auto rightBranch = makeSignerEqualsTree(rsp::KeyPair::generateP256().nodeID());
        const auto tree = makeOrTree(leftBranch, rightBranch);
        const auto reduced = rsp::reduceRequirementTree(tree, std::vector<rsp::Endorsement>{endorsement});

        require(reduced.node_type_case() == rsp::proto::ERDAbstractSyntaxTree::kOr,
            "reduction should keep unmet alternatives in an or-tree");
        require(reduced.or_().lhs().node_type_case() == rsp::proto::ERDAbstractSyntaxTree::kValueEquals,
            "reduction should drop the matched portion of the partially satisfied branch");
        require(reduced.or_().rhs().node_type_case() == rsp::proto::ERDAbstractSyntaxTree::kSignerEquals,
            "reduction should preserve the untouched alternative branch");
    }

    void testReduceRequirementTreeClearsFullySatisfiedTree() {
        rsp::KeyPair endorsementServiceKey = rsp::KeyPair::generateP256();
        rsp::KeyPair subjectKey = rsp::KeyPair::generateP256();
        const rsp::GUID matchingType("00112233-4455-6677-8899-aabbccddeeff");
        const rsp::Endorsement endorsement = makeTestEndorsement(endorsementServiceKey, subjectKey, matchingType, "network-access");

        const auto tree = makeAndTree(makeTypeEqualsTree(matchingType), makeValueEqualsTree("network-access"));
        const auto reduced = rsp::reduceRequirementTree(tree, std::vector<rsp::Endorsement>{endorsement});

        require(isEmptyTree(reduced),
            "reduction should clear a fully satisfied tree");
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
        testRequirementSignerEquals();
        testRequirementAndOrEqualsNodes();
        testRequirementComplexAst();
        testReduceRequirementTreeEliminatesMatchedAndBranch();
        testReduceRequirementTreeEliminatesSatisfiedOrTree();
        testReduceRequirementTreePreservesUnmetAlternatives();
        testReduceRequirementTreeClearsFullySatisfiedTree();

        std::cout << "endorsement_test passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "endorsement_test failed: " << exception.what() << '\n';
        return 1;
    }
}