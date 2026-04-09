#include "common/endorsement/endorsement.hpp"

#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>

namespace rsp {

namespace {

std::runtime_error makeError(const char* message) {
    return std::runtime_error(message);
}

void appendUint64(std::string& bytes, uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        bytes.push_back(static_cast<char>((value >> shift) & 0xFFULL));
    }
}

std::string serializeGuidBytes(const GUID& guid) {
    std::string bytes;
    bytes.reserve(16);
    appendUint64(bytes, guid.high());
    appendUint64(bytes, guid.low());
    return bytes;
}

GUID deserializeGuidBytes(const std::string& bytes, const char* fieldName) {
    if (bytes.size() != 16) {
        throw makeError(fieldName);
    }

    uint64_t high = 0;
    uint64_t low = 0;
    for (size_t index = 0; index < 8; ++index) {
        high = (high << 8) | static_cast<uint64_t>(static_cast<uint8_t>(bytes[index]));
        low = (low << 8) | static_cast<uint64_t>(static_cast<uint8_t>(bytes[index + 8]));
    }

    return GUID(high, low);
}

Buffer stringToBuffer(const std::string& bytes) {
    return Buffer(reinterpret_cast<const uint8_t*>(bytes.data()), static_cast<uint32_t>(bytes.size()));
}

std::string bufferToString(const Buffer& buffer) {
    if (buffer.empty()) {
        return std::string();
    }

    return std::string(reinterpret_cast<const char*>(buffer.data()), buffer.size());
}

Buffer serializeDeterministic(const google::protobuf::MessageLite& message) {
    std::string serialized;
    serialized.resize(message.ByteSizeLong());

    google::protobuf::io::ArrayOutputStream arrayOutputStream(serialized.data(), static_cast<int>(serialized.size()));
    google::protobuf::io::CodedOutputStream codedOutputStream(&arrayOutputStream);
    codedOutputStream.SetSerializationDeterministic(true);

    if (!message.SerializeToCodedStream(&codedOutputStream) || codedOutputStream.HadError() ||
        static_cast<size_t>(codedOutputStream.ByteCount()) != serialized.size()) {
        throw makeError("failed to serialize endorsement protobuf");
    }

    return stringToBuffer(serialized);
}

void validateUuidLike(const std::string& bytes, const char* fieldName) {
    if (bytes.size() != 16) {
        throw makeError(fieldName);
    }
}

void populateUuid(rsp::proto::Uuid* target, const GUID& guid) {
    target->set_value(serializeGuidBytes(guid));
}

void populateNodeId(rsp::proto::NodeId* target, const NodeID& nodeId) {
    target->set_value(serializeGuidBytes(nodeId));
}

NodeID parseNodeId(const rsp::proto::NodeId& nodeId, const char* fieldName) {
    return NodeID(deserializeGuidBytes(nodeId.value(), fieldName));
}

GUID parseUuid(const rsp::proto::Uuid& uuid, const char* fieldName) {
    return deserializeGuidBytes(uuid.value(), fieldName);
}

GUID parseEndorsementType(const rsp::proto::EndorsementType& endorsementType, const char* fieldName) {
    return deserializeGuidBytes(endorsementType.value(), fieldName);
}

void setMessageFields(rsp::proto::Endorsement* message, NodeID subject, NodeID endorsementService,
                      GUID endorsementType, Buffer endorsementValue, DateTime validUntil,
                      Buffer signature) {
    populateNodeId(message->mutable_subject(), subject);
    populateNodeId(message->mutable_endorsement_service(), endorsementService);
    populateUuid(message->mutable_endorsement_type(), endorsementType);
    message->set_endorsement_value(bufferToString(endorsementValue));
    message->mutable_valid_until()->set_milliseconds_since_epoch(validUntil.millisecondsSinceEpoch());
    message->set_signature(bufferToString(signature));
}

bool evaluateRequirementTree(const rsp::proto::ERDAbstractSyntaxTree& tree,
                             const Endorsement& endorsement) {
    switch (tree.node_type_case()) {
        case rsp::proto::ERDAbstractSyntaxTree::kEquals:
            return tree.equals().has_lhs() && tree.equals().has_rhs() &&
                   evaluateRequirementTree(tree.equals().lhs(), endorsement) ==
                       evaluateRequirementTree(tree.equals().rhs(), endorsement);
        case rsp::proto::ERDAbstractSyntaxTree::kAnd:
            return tree.and_().has_lhs() && tree.and_().has_rhs() &&
                   evaluateRequirementTree(tree.and_().lhs(), endorsement) &&
                   evaluateRequirementTree(tree.and_().rhs(), endorsement);
        case rsp::proto::ERDAbstractSyntaxTree::kOr:
            return tree.or_().has_lhs() && tree.or_().has_rhs() &&
                   (evaluateRequirementTree(tree.or_().lhs(), endorsement) ||
                    evaluateRequirementTree(tree.or_().rhs(), endorsement));
        case rsp::proto::ERDAbstractSyntaxTree::kTypeEquals:
            return tree.type_equals().has_type() &&
                   endorsement.endorsementType() ==
                       parseEndorsementType(tree.type_equals().type(), "invalid requirement endorsement type length");
        case rsp::proto::ERDAbstractSyntaxTree::kValueEquals:
            return bufferToString(endorsement.endorsementValue()) == tree.value_equals().value();
        case rsp::proto::ERDAbstractSyntaxTree::kSignerEquals:
            return tree.signer_equals().has_signer() &&
                   endorsement.endorsementService() ==
                       parseNodeId(tree.signer_equals().signer(), "invalid requirement signer length");
        case rsp::proto::ERDAbstractSyntaxTree::NODE_TYPE_NOT_SET:
            return false;
    }

    return false;
}

bool isEmptyTree(const rsp::proto::ERDAbstractSyntaxTree& tree) {
    return tree.node_type_case() == rsp::proto::ERDAbstractSyntaxTree::NODE_TYPE_NOT_SET;
}

bool anyEndorsementMatchesTree(const rsp::proto::ERDAbstractSyntaxTree& tree,
                               const std::vector<Endorsement>& endorsements) {
    for (const auto& endorsement : endorsements) {
        if (evaluateRequirementTree(tree, endorsement)) {
            return true;
        }
    }

    return false;
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

rsp::proto::ERDAbstractSyntaxTree reduceRequirementTreeImpl(const rsp::proto::ERDAbstractSyntaxTree& tree,
                                                            const std::vector<Endorsement>& endorsements) {
    if (anyEndorsementMatchesTree(tree, endorsements)) {
        return rsp::proto::ERDAbstractSyntaxTree();
    }

    switch (tree.node_type_case()) {
        case rsp::proto::ERDAbstractSyntaxTree::kEquals: {
            if (!tree.equals().has_lhs() || !tree.equals().has_rhs()) {
                return tree;
            }

            const auto reducedLeft = reduceRequirementTreeImpl(tree.equals().lhs(), endorsements);
            const auto reducedRight = reduceRequirementTreeImpl(tree.equals().rhs(), endorsements);
            if (isEmptyTree(reducedLeft) && isEmptyTree(reducedRight)) {
                return rsp::proto::ERDAbstractSyntaxTree();
            }

            if (isEmptyTree(reducedLeft)) {
                return reducedRight;
            }

            if (isEmptyTree(reducedRight)) {
                return reducedLeft;
            }

            return makeEqualsTree(reducedLeft, reducedRight);
        }
        case rsp::proto::ERDAbstractSyntaxTree::kAnd: {
            if (!tree.and_().has_lhs() || !tree.and_().has_rhs()) {
                return tree;
            }

            const auto reducedLeft = reduceRequirementTreeImpl(tree.and_().lhs(), endorsements);
            const auto reducedRight = reduceRequirementTreeImpl(tree.and_().rhs(), endorsements);
            if (isEmptyTree(reducedLeft)) {
                return reducedRight;
            }

            if (isEmptyTree(reducedRight)) {
                return reducedLeft;
            }

            return makeAndTree(reducedLeft, reducedRight);
        }
        case rsp::proto::ERDAbstractSyntaxTree::kOr: {
            if (!tree.or_().has_lhs() || !tree.or_().has_rhs()) {
                return tree;
            }

            const auto reducedLeft = reduceRequirementTreeImpl(tree.or_().lhs(), endorsements);
            const auto reducedRight = reduceRequirementTreeImpl(tree.or_().rhs(), endorsements);
            if (isEmptyTree(reducedLeft) || isEmptyTree(reducedRight)) {
                return rsp::proto::ERDAbstractSyntaxTree();
            }

            return makeOrTree(reducedLeft, reducedRight);
        }
        case rsp::proto::ERDAbstractSyntaxTree::kTypeEquals:
        case rsp::proto::ERDAbstractSyntaxTree::kValueEquals:
        case rsp::proto::ERDAbstractSyntaxTree::kSignerEquals:
        case rsp::proto::ERDAbstractSyntaxTree::NODE_TYPE_NOT_SET:
            return tree;
    }

    return tree;
}

}  // namespace

Endorsement::Endorsement() = default;

Endorsement::Endorsement(NodeID subject, NodeID endorsementService, GUID endorsementType,
                         Buffer endorsementValue, DateTime validUntil, Buffer signature) {
    setMessageFields(&message_, subject, endorsementService, endorsementType,
                     std::move(endorsementValue), validUntil, std::move(signature));
}

Endorsement::Endorsement(rsp::proto::Endorsement message) : message_(std::move(message)) {
}

Endorsement Endorsement::createSigned(const KeyPair& endorsementServiceKeyPair, const NodeID& subject,
                                      const GUID& endorsementType, const Buffer& endorsementValue,
                                      const DateTime& validUntil) {
    Endorsement endorsement(subject, endorsementServiceKeyPair.nodeID(), endorsementType,
                            endorsementValue, validUntil, Buffer());
    endorsement.message_.set_signature(bufferToString(endorsementServiceKeyPair.sign(endorsement.serializeUnsigned())));
    return endorsement;
}

Endorsement Endorsement::deserialize(const Buffer& serialized) {
    rsp::proto::Endorsement message;
    if (!message.ParseFromArray(serialized.data(), static_cast<int>(serialized.size()))) {
        throw makeError("failed to deserialize endorsement protobuf");
    }

    validateMessage(message, true);
    return Endorsement(std::move(message));
}

Buffer Endorsement::serialize() const {
    validateMessage(message_, true);
    return serializeDeterministic(message_);
}

bool Endorsement::verifySignature(const KeyPair& endorsementServiceKeyPair) const {
    validateMessage(message_, true);
    if (endorsementServiceKeyPair.nodeID() != endorsementService()) {
        return false;
    }

    return endorsementServiceKeyPair.verify(serializeUnsigned(), signature());
}

NodeID Endorsement::subject() const {
    validateMessage(message_, true);
    return parseNodeId(message_.subject(), "invalid endorsement subject length");
}

NodeID Endorsement::endorsementService() const {
    validateMessage(message_, true);
    return parseNodeId(message_.endorsement_service(), "invalid endorsement service length");
}

GUID Endorsement::endorsementType() const {
    validateMessage(message_, true);
    return parseUuid(message_.endorsement_type(), "invalid endorsement type length");
}

Buffer Endorsement::endorsementValue() const {
    validateMessage(message_, true);
    return stringToBuffer(message_.endorsement_value());
}

DateTime Endorsement::validUntil() const {
    validateMessage(message_, true);
    return DateTime::fromMillisecondsSinceEpoch(message_.valid_until().milliseconds_since_epoch());
}

Buffer Endorsement::signature() const {
    validateMessage(message_, true);
    return stringToBuffer(message_.signature());
}

Buffer Endorsement::serializeUnsigned() const {
    rsp::proto::Endorsement unsignedMessage(message_);
    unsignedMessage.clear_signature();
    validateMessage(unsignedMessage, false);
    return serializeDeterministic(unsignedMessage);
}

void Endorsement::validateMessage(const rsp::proto::Endorsement& message, bool requireSignature) {
    if (!message.has_subject()) {
        throw makeError("endorsement missing subject");
    }

    if (!message.has_endorsement_service()) {
        throw makeError("endorsement missing endorsement service");
    }

    if (!message.has_endorsement_type()) {
        throw makeError("endorsement missing endorsement type");
    }

    if (!message.has_valid_until()) {
        throw makeError("endorsement missing valid-until time");
    }

    validateUuidLike(message.subject().value(), "invalid endorsement subject length");
    validateUuidLike(message.endorsement_service().value(), "invalid endorsement service length");
    validateUuidLike(message.endorsement_type().value(), "invalid endorsement type length");

    if (requireSignature && message.signature().empty()) {
        throw makeError("endorsement missing signature");
    }
}

bool endorsementMatchesRequirement(const rsp::proto::EndorsementNeeded& requirement,
                                  const Endorsement& endorsement) {
    if (!requirement.has_tree()) {
        return false;
    }

    try {
        return evaluateRequirementTree(requirement.tree(), endorsement);
    } catch (const std::runtime_error&) {
        return false;
    }
}

rsp::proto::ERDAbstractSyntaxTree reduceRequirementTree(const rsp::proto::ERDAbstractSyntaxTree& tree,
                                                        const std::vector<Endorsement>& endorsements) {
    try {
        return reduceRequirementTreeImpl(tree, endorsements);
    } catch (const std::runtime_error&) {
        return tree;
    }
}

}  // namespace rsp