#include "common/endorsement/endorsement.hpp"
#include "common/endorsement/field_resolver.hpp"
#include "resource_manager/schema_registry.hpp"

#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>

namespace rsp {

namespace {

constexpr char kSerializedEndorsementMagic[] = {'R', 'S', 'E', '1'};
constexpr size_t kSerializedEndorsementMagicSize = sizeof(kSerializedEndorsementMagic);

std::runtime_error makeError(const char* message) {
    return std::runtime_error(message);
}

void appendUint32(std::string& bytes, uint32_t value) {
    bytes.push_back(static_cast<char>((value >> 24) & 0xFFU));
    bytes.push_back(static_cast<char>((value >> 16) & 0xFFU));
    bytes.push_back(static_cast<char>((value >> 8) & 0xFFU));
    bytes.push_back(static_cast<char>(value & 0xFFU));
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

class BufferReader {
public:
    explicit BufferReader(const Buffer& buffer)
        : data_(buffer.data()), size_(static_cast<size_t>(buffer.size())), offset_(0) {
    }

    void expectMagic(const char* magic, size_t length) {
        const std::string bytes = readBytes(length, "serialized endorsement missing magic");
        if (std::memcmp(bytes.data(), magic, length) != 0) {
            throw makeError("serialized endorsement has invalid magic");
        }
    }

    uint32_t readUint32(const char* fieldName) {
        const std::string bytes = readBytes(sizeof(uint32_t), fieldName);
        return (static_cast<uint32_t>(static_cast<uint8_t>(bytes[0])) << 24) |
               (static_cast<uint32_t>(static_cast<uint8_t>(bytes[1])) << 16) |
               (static_cast<uint32_t>(static_cast<uint8_t>(bytes[2])) << 8) |
               static_cast<uint32_t>(static_cast<uint8_t>(bytes[3]));
    }

    uint64_t readUint64(const char* fieldName) {
        const std::string bytes = readBytes(sizeof(uint64_t), fieldName);
        uint64_t value = 0;
        for (unsigned char byte : bytes) {
            value = (value << 8) | static_cast<uint64_t>(byte);
        }
        return value;
    }

    std::string readBytes(size_t count, const char* fieldName) {
        if (offset_ + count > size_) {
            throw makeError(fieldName);
        }

        std::string bytes(reinterpret_cast<const char*>(data_ + offset_), count);
        offset_ += count;
        return bytes;
    }

    void expectDone() const {
        if (offset_ != size_) {
            throw makeError("serialized endorsement has trailing bytes");
        }
    }

private:
    const uint8_t* data_;
    size_t size_;
    size_t offset_;
};

Buffer serializeBinary(const rsp::proto::Endorsement& message) {
    std::string serialized;
    serialized.reserve(kSerializedEndorsementMagicSize + 16 + 16 + 16 + 8 + 4 + 4 +
                       message.endorsement_value().size() + message.signature().size());

    serialized.append(kSerializedEndorsementMagic, kSerializedEndorsementMagicSize);
    serialized.append(message.subject().value());
    serialized.append(message.endorsement_service().value());
    serialized.append(message.endorsement_type().value());
    appendUint64(serialized, message.valid_until().milliseconds_since_epoch());
    appendUint32(serialized, static_cast<uint32_t>(message.endorsement_value().size()));
    appendUint32(serialized, static_cast<uint32_t>(message.signature().size()));
    serialized.append(message.endorsement_value());
    serialized.append(message.signature());
    return stringToBuffer(serialized);
}

rsp::proto::Endorsement deserializeBinary(const Buffer& serialized) {
    BufferReader reader(serialized);
    reader.expectMagic(kSerializedEndorsementMagic, kSerializedEndorsementMagicSize);

    rsp::proto::Endorsement message;
    message.mutable_subject()->set_value(reader.readBytes(16, "serialized endorsement missing subject"));
    message.mutable_endorsement_service()->set_value(
        reader.readBytes(16, "serialized endorsement missing endorsement service"));
    message.mutable_endorsement_type()->set_value(
        reader.readBytes(16, "serialized endorsement missing endorsement type"));
    message.mutable_valid_until()->set_milliseconds_since_epoch(
        reader.readUint64("serialized endorsement missing valid-until time"));

    const uint32_t endorsementValueSize = reader.readUint32("serialized endorsement missing value size");
    const uint32_t signatureSize = reader.readUint32("serialized endorsement missing signature size");
    message.set_endorsement_value(
        reader.readBytes(endorsementValueSize, "serialized endorsement value truncated"));
    message.set_signature(reader.readBytes(signatureSize, "serialized endorsement signature truncated"));
    reader.expectDone();
    return message;
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

NodeID parseMessageNodeId(const rsp::proto::NodeId& nodeId, const char* fieldName) {
    if (nodeId.value().size() != 16) {
        throw makeError(fieldName);
    }

    uint64_t high = 0;
    uint64_t low = 0;
    std::memcpy(&high, nodeId.value().data(), sizeof(high));
    std::memcpy(&low, nodeId.value().data() + sizeof(high), sizeof(low));
    return NodeID(high, low);
}

NodeID parseSignatureSignerNodeId(const rsp::proto::NodeId& nodeId, const char* fieldName) {
    return parseNodeId(nodeId, fieldName);
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

bool evaluateEndorsementTree(const rsp::proto::ERDASTEndTree& tree,
                             const Endorsement& endorsement) {
    switch (tree.node_type_case()) {
        case rsp::proto::ERDASTEndTree::kEquals:
            return tree.equals().has_lhs() && tree.equals().has_rhs() &&
                   evaluateEndorsementTree(tree.equals().lhs(), endorsement) ==
                       evaluateEndorsementTree(tree.equals().rhs(), endorsement);
        case rsp::proto::ERDASTEndTree::kAnd:
            return tree.and_().has_lhs() && tree.and_().has_rhs() &&
                   evaluateEndorsementTree(tree.and_().lhs(), endorsement) &&
                   evaluateEndorsementTree(tree.and_().rhs(), endorsement);
        case rsp::proto::ERDASTEndTree::kOr:
            return tree.or_().has_lhs() && tree.or_().has_rhs() &&
                   (evaluateEndorsementTree(tree.or_().lhs(), endorsement) ||
                    evaluateEndorsementTree(tree.or_().rhs(), endorsement));
        case rsp::proto::ERDASTEndTree::kAllOf:
            if (tree.all_of().terms_size() == 0) {
                return false;
            }

            for (const auto& term : tree.all_of().terms()) {
                if (!evaluateEndorsementTree(term, endorsement)) {
                    return false;
                }
            }

            return true;
        case rsp::proto::ERDASTEndTree::kAnyOf:
            for (const auto& term : tree.any_of().terms()) {
                if (evaluateEndorsementTree(term, endorsement)) {
                    return true;
                }
            }

            return false;
        case rsp::proto::ERDASTEndTree::kTypeEquals:
            return tree.type_equals().has_type() &&
                   endorsement.endorsementType() ==
                       parseEndorsementType(tree.type_equals().type(), "invalid requirement endorsement type length");
        case rsp::proto::ERDASTEndTree::kValueEquals:
            return bufferToString(endorsement.endorsementValue()) == tree.value_equals().value();
        case rsp::proto::ERDASTEndTree::kSignerEquals:
            return tree.signer_equals().has_signer() &&
                   endorsement.endorsementService() ==
                       parseNodeId(tree.signer_equals().signer(), "invalid requirement signer length");
        case rsp::proto::ERDASTEndTree::kTrueValue:
            return true;
        case rsp::proto::ERDASTEndTree::kFalseValue:
            return false;
        case rsp::proto::ERDASTEndTree::NODE_TYPE_NOT_SET:
            return false;
    }

    return false;
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
        case rsp::proto::ERDAbstractSyntaxTree::kAllOf:
            if (tree.all_of().terms_size() == 0) {
                return false;
            }

            for (const auto& term : tree.all_of().terms()) {
                if (!evaluateRequirementTree(term, endorsement)) {
                    return false;
                }
            }

            return true;
        case rsp::proto::ERDAbstractSyntaxTree::kAnyOf:
            for (const auto& term : tree.any_of().terms()) {
                if (evaluateRequirementTree(term, endorsement)) {
                    return true;
                }
            }

            return false;
        case rsp::proto::ERDAbstractSyntaxTree::kEndorsement:
            return tree.endorsement().has_tree() &&
                   evaluateEndorsementTree(tree.endorsement().tree(), endorsement);
        case rsp::proto::ERDAbstractSyntaxTree::kMessage:
            return false;
        case rsp::proto::ERDAbstractSyntaxTree::kTrueValue:
            return true;
        case rsp::proto::ERDAbstractSyntaxTree::kFalseValue:
            return false;
        case rsp::proto::ERDAbstractSyntaxTree::NODE_TYPE_NOT_SET:
            return false;
    }

    return false;
}

bool evaluateMessageTree(const rsp::proto::ERDASTMessageTree& tree,
                         const rsp::proto::RSPMessage& message,
                         const rsp::resource_manager::SchemaSnapshot* snap) {
    switch (tree.node_type_case()) {
        case rsp::proto::ERDASTMessageTree::kEquals:
            return tree.equals().has_lhs() && tree.equals().has_rhs() &&
                   evaluateMessageTree(tree.equals().lhs(), message, snap) ==
                       evaluateMessageTree(tree.equals().rhs(), message, snap);
        case rsp::proto::ERDASTMessageTree::kAnd:
            return tree.and_().has_lhs() && tree.and_().has_rhs() &&
                   evaluateMessageTree(tree.and_().lhs(), message, snap) &&
                   evaluateMessageTree(tree.and_().rhs(), message, snap);
        case rsp::proto::ERDASTMessageTree::kOr:
            return tree.or_().has_lhs() && tree.or_().has_rhs() &&
                   (evaluateMessageTree(tree.or_().lhs(), message, snap) ||
                    evaluateMessageTree(tree.or_().rhs(), message, snap));
        case rsp::proto::ERDASTMessageTree::kAllOf:
            if (tree.all_of().terms_size() == 0) {
                return false;
            }

            for (const auto& term : tree.all_of().terms()) {
                if (!evaluateMessageTree(term, message, snap)) {
                    return false;
                }
            }

            return true;
        case rsp::proto::ERDASTMessageTree::kAnyOf:
            for (const auto& term : tree.any_of().terms()) {
                if (evaluateMessageTree(term, message, snap)) {
                    return true;
                }
            }

            return false;
        case rsp::proto::ERDASTMessageTree::kFieldEquals: {
            if (!tree.field_equals().has_path() || !tree.field_equals().has_value()) return false;
            auto resolved = rsp::endorsement::resolveFieldPath(tree.field_equals().path(), message, snap);
            return rsp::endorsement::resolvedValueEquals(resolved, tree.field_equals().value());
        }
        case rsp::proto::ERDASTMessageTree::kFieldExists: {
            if (!tree.field_exists().has_path()) return false;
            auto resolved = rsp::endorsement::resolveFieldPath(tree.field_exists().path(), message, snap);
            return rsp::endorsement::resolvedValuePresent(resolved);
        }
        case rsp::proto::ERDASTMessageTree::kFieldContains: {
            if (!tree.field_contains().has_path() || !tree.field_contains().has_sub_path() ||
                !tree.field_contains().has_value()) return false;
            auto elements = rsp::endorsement::resolveRepeatedMessages(
                tree.field_contains().path(), message, snap);
            for (const auto* elem : elements) {
                auto resolved = rsp::endorsement::resolveFieldPath(
                    tree.field_contains().sub_path(), *elem, snap);
                if (rsp::endorsement::resolvedValueEquals(resolved, tree.field_contains().value())) {
                    return true;
                }
            }
            return false;
        }
        case rsp::proto::ERDASTMessageTree::kTrueValue:
            return true;
        case rsp::proto::ERDASTMessageTree::kFalseValue:
            return false;
        case rsp::proto::ERDASTMessageTree::NODE_TYPE_NOT_SET:
            return false;
    }

    return false;
}

bool evaluateMessageRequirementTree(const rsp::proto::ERDAbstractSyntaxTree& tree,
                                    const rsp::proto::RSPMessage& message,
                                    const rsp::resource_manager::SchemaSnapshot* snap) {
    switch (tree.node_type_case()) {
        case rsp::proto::ERDAbstractSyntaxTree::kEquals:
            return tree.equals().has_lhs() && tree.equals().has_rhs() &&
                   evaluateMessageRequirementTree(tree.equals().lhs(), message, snap) ==
                       evaluateMessageRequirementTree(tree.equals().rhs(), message, snap);
        case rsp::proto::ERDAbstractSyntaxTree::kAnd:
            return tree.and_().has_lhs() && tree.and_().has_rhs() &&
                   evaluateMessageRequirementTree(tree.and_().lhs(), message, snap) &&
                   evaluateMessageRequirementTree(tree.and_().rhs(), message, snap);
        case rsp::proto::ERDAbstractSyntaxTree::kOr:
            return tree.or_().has_lhs() && tree.or_().has_rhs() &&
                   (evaluateMessageRequirementTree(tree.or_().lhs(), message, snap) ||
                    evaluateMessageRequirementTree(tree.or_().rhs(), message, snap));
        case rsp::proto::ERDAbstractSyntaxTree::kAllOf:
            if (tree.all_of().terms_size() == 0) {
                return false;
            }

            for (const auto& term : tree.all_of().terms()) {
                if (!evaluateMessageRequirementTree(term, message, snap)) {
                    return false;
                }
            }

            return true;
        case rsp::proto::ERDAbstractSyntaxTree::kAnyOf:
            for (const auto& term : tree.any_of().terms()) {
                if (evaluateMessageRequirementTree(term, message, snap)) {
                    return true;
                }
            }

            return false;
        case rsp::proto::ERDAbstractSyntaxTree::kMessage:
            return tree.message().has_tree() &&
                   evaluateMessageTree(tree.message().tree(), message, snap);
        case rsp::proto::ERDAbstractSyntaxTree::kEndorsement:
            return false;
        case rsp::proto::ERDAbstractSyntaxTree::kTrueValue:
            return true;
        case rsp::proto::ERDAbstractSyntaxTree::kFalseValue:
            return false;
        case rsp::proto::ERDAbstractSyntaxTree::NODE_TYPE_NOT_SET:
            return false;
    }

    return false;
}

bool isEmptyTree(const rsp::proto::ERDAbstractSyntaxTree& tree) {
    return tree.node_type_case() == rsp::proto::ERDAbstractSyntaxTree::NODE_TYPE_NOT_SET;
}

bool isFalseTree(const rsp::proto::ERDAbstractSyntaxTree& tree) {
    return tree.node_type_case() == rsp::proto::ERDAbstractSyntaxTree::kFalseValue;
}

rsp::proto::ERDAbstractSyntaxTree makeFalseTree() {
    rsp::proto::ERDAbstractSyntaxTree tree;
    tree.mutable_false_value();
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

enum class ConstantTreeValue {
    kUnknown,
    kTrue,
    kFalse,
};

ConstantTreeValue getConstantTreeValue(const rsp::proto::ERDAbstractSyntaxTree& tree) {
    switch (tree.node_type_case()) {
        case rsp::proto::ERDAbstractSyntaxTree::kTrueValue:
            return ConstantTreeValue::kTrue;
        case rsp::proto::ERDAbstractSyntaxTree::kFalseValue:
            return ConstantTreeValue::kFalse;
        case rsp::proto::ERDAbstractSyntaxTree::kEquals: {
            if (!tree.equals().has_lhs() || !tree.equals().has_rhs()) {
                return ConstantTreeValue::kUnknown;
            }

            const auto lhsValue = getConstantTreeValue(tree.equals().lhs());
            const auto rhsValue = getConstantTreeValue(tree.equals().rhs());
            if (lhsValue == ConstantTreeValue::kUnknown || rhsValue == ConstantTreeValue::kUnknown) {
                return ConstantTreeValue::kUnknown;
            }

            return lhsValue == rhsValue ? ConstantTreeValue::kTrue : ConstantTreeValue::kFalse;
        }
        case rsp::proto::ERDAbstractSyntaxTree::kAnd: {
            if (!tree.and_().has_lhs() || !tree.and_().has_rhs()) {
                return ConstantTreeValue::kUnknown;
            }

            const auto lhsValue = getConstantTreeValue(tree.and_().lhs());
            const auto rhsValue = getConstantTreeValue(tree.and_().rhs());
            if (lhsValue == ConstantTreeValue::kFalse || rhsValue == ConstantTreeValue::kFalse) {
                return ConstantTreeValue::kFalse;
            }

            if (lhsValue == ConstantTreeValue::kTrue && rhsValue == ConstantTreeValue::kTrue) {
                return ConstantTreeValue::kTrue;
            }

            return ConstantTreeValue::kUnknown;
        }
        case rsp::proto::ERDAbstractSyntaxTree::kOr: {
            if (!tree.or_().has_lhs() || !tree.or_().has_rhs()) {
                return ConstantTreeValue::kUnknown;
            }

            const auto lhsValue = getConstantTreeValue(tree.or_().lhs());
            const auto rhsValue = getConstantTreeValue(tree.or_().rhs());
            if (lhsValue == ConstantTreeValue::kTrue || rhsValue == ConstantTreeValue::kTrue) {
                return ConstantTreeValue::kTrue;
            }

            if (lhsValue == ConstantTreeValue::kFalse && rhsValue == ConstantTreeValue::kFalse) {
                return ConstantTreeValue::kFalse;
            }

            return ConstantTreeValue::kUnknown;
        }
        case rsp::proto::ERDAbstractSyntaxTree::kAllOf: {
            if (tree.all_of().terms_size() == 0) {
                return ConstantTreeValue::kUnknown;
            }

            bool allTrue = true;
            for (const auto& term : tree.all_of().terms()) {
                const auto termValue = getConstantTreeValue(term);
                if (termValue == ConstantTreeValue::kFalse) {
                    return ConstantTreeValue::kFalse;
                }

                if (termValue != ConstantTreeValue::kTrue) {
                    allTrue = false;
                }
            }

            return allTrue ? ConstantTreeValue::kTrue : ConstantTreeValue::kUnknown;
        }
        case rsp::proto::ERDAbstractSyntaxTree::kAnyOf: {
            if (tree.any_of().terms_size() == 0) {
                return ConstantTreeValue::kUnknown;
            }

            bool allFalse = true;
            for (const auto& term : tree.any_of().terms()) {
                const auto termValue = getConstantTreeValue(term);
                if (termValue == ConstantTreeValue::kTrue) {
                    return ConstantTreeValue::kTrue;
                }

                if (termValue != ConstantTreeValue::kFalse) {
                    allFalse = false;
                }
            }

            return allFalse ? ConstantTreeValue::kFalse : ConstantTreeValue::kUnknown;
        }
        case rsp::proto::ERDAbstractSyntaxTree::kEndorsement:
        case rsp::proto::ERDAbstractSyntaxTree::kMessage:
        case rsp::proto::ERDAbstractSyntaxTree::NODE_TYPE_NOT_SET:
            return ConstantTreeValue::kUnknown;
    }

    return ConstantTreeValue::kUnknown;
}

bool isMessagePredicateNode(const rsp::proto::ERDAbstractSyntaxTree& tree) {
    switch (tree.node_type_case()) {
        case rsp::proto::ERDAbstractSyntaxTree::kMessage:
            return true;
        case rsp::proto::ERDAbstractSyntaxTree::kEquals:
        case rsp::proto::ERDAbstractSyntaxTree::kAnd:
        case rsp::proto::ERDAbstractSyntaxTree::kOr:
        case rsp::proto::ERDAbstractSyntaxTree::kAllOf:
        case rsp::proto::ERDAbstractSyntaxTree::kAnyOf:
        case rsp::proto::ERDAbstractSyntaxTree::kEndorsement:
        case rsp::proto::ERDAbstractSyntaxTree::kTrueValue:
        case rsp::proto::ERDAbstractSyntaxTree::kFalseValue:
        case rsp::proto::ERDAbstractSyntaxTree::NODE_TYPE_NOT_SET:
            return false;
    }

    return false;
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
                                                            const std::vector<Endorsement>& endorsements,
                                                            const rsp::proto::RSPMessage* message,
                                                            const rsp::resource_manager::SchemaSnapshot* snap) {
    const auto constantValue = getConstantTreeValue(tree);
    if (constantValue == ConstantTreeValue::kTrue) {
        return rsp::proto::ERDAbstractSyntaxTree();
    }

    if (constantValue == ConstantTreeValue::kFalse) {
        return makeFalseTree();
    }

    if (anyEndorsementMatchesTree(tree, endorsements)) {
        return rsp::proto::ERDAbstractSyntaxTree();
    }

    if (message != nullptr && isMessagePredicateNode(tree)) {
        return evaluateMessageRequirementTree(tree, *message, snap) ? rsp::proto::ERDAbstractSyntaxTree()
                                                              : makeFalseTree();
    }

    switch (tree.node_type_case()) {
        case rsp::proto::ERDAbstractSyntaxTree::kEquals: {
            if (!tree.equals().has_lhs() || !tree.equals().has_rhs()) {
                return tree;
            }

            const auto reducedLeft = reduceRequirementTreeImpl(tree.equals().lhs(), endorsements, message, snap);
            const auto reducedRight = reduceRequirementTreeImpl(tree.equals().rhs(), endorsements, message, snap);
            if (isEmptyTree(reducedLeft) && isEmptyTree(reducedRight)) {
                return rsp::proto::ERDAbstractSyntaxTree();
            }

            if (isEmptyTree(reducedLeft)) {
                return reducedRight;
            }

            if (isEmptyTree(reducedRight)) {
                return reducedLeft;
            }

            if (isFalseTree(reducedLeft) && isFalseTree(reducedRight)) {
                return rsp::proto::ERDAbstractSyntaxTree();
            }

            return makeEqualsTree(reducedLeft, reducedRight);
        }
        case rsp::proto::ERDAbstractSyntaxTree::kAnd: {
            if (!tree.and_().has_lhs() || !tree.and_().has_rhs()) {
                return tree;
            }

            const auto reducedLeft = reduceRequirementTreeImpl(tree.and_().lhs(), endorsements, message, snap);
            const auto reducedRight = reduceRequirementTreeImpl(tree.and_().rhs(), endorsements, message, snap);
            if (isEmptyTree(reducedLeft)) {
                return reducedRight;
            }

            if (isEmptyTree(reducedRight)) {
                return reducedLeft;
            }

            if (isFalseTree(reducedLeft) || isFalseTree(reducedRight)) {
                return makeFalseTree();
            }

            return makeAndTree(reducedLeft, reducedRight);
        }
        case rsp::proto::ERDAbstractSyntaxTree::kOr: {
            if (!tree.or_().has_lhs() || !tree.or_().has_rhs()) {
                return tree;
            }

            const auto reducedLeft = reduceRequirementTreeImpl(tree.or_().lhs(), endorsements, message, snap);
            const auto reducedRight = reduceRequirementTreeImpl(tree.or_().rhs(), endorsements, message, snap);
            if (isEmptyTree(reducedLeft) || isEmptyTree(reducedRight)) {
                return rsp::proto::ERDAbstractSyntaxTree();
            }

            if (isFalseTree(reducedLeft)) {
                return reducedRight;
            }

            if (isFalseTree(reducedRight)) {
                return reducedLeft;
            }

            return makeOrTree(reducedLeft, reducedRight);
        }
        case rsp::proto::ERDAbstractSyntaxTree::kAllOf: {
            if (tree.all_of().terms_size() == 0) {
                return tree;
            }

            std::vector<rsp::proto::ERDAbstractSyntaxTree> remainingTerms;
            remainingTerms.reserve(static_cast<std::size_t>(tree.all_of().terms_size()));
            for (const auto& term : tree.all_of().terms()) {
                auto reducedTerm = reduceRequirementTreeImpl(term, endorsements, message, snap);
                if (isEmptyTree(reducedTerm)) {
                    continue;
                }

                if (isFalseTree(reducedTerm)) {
                    return makeFalseTree();
                }

                remainingTerms.push_back(std::move(reducedTerm));
            }

            if (remainingTerms.empty()) {
                return rsp::proto::ERDAbstractSyntaxTree();
            }

            if (remainingTerms.size() == 1) {
                return remainingTerms.front();
            }

            return makeAllOfTree(remainingTerms);
        }
        case rsp::proto::ERDAbstractSyntaxTree::kAnyOf: {
            if (tree.any_of().terms_size() == 0) {
                return tree;
            }

            std::vector<rsp::proto::ERDAbstractSyntaxTree> remainingTerms;
            remainingTerms.reserve(static_cast<std::size_t>(tree.any_of().terms_size()));
            for (const auto& term : tree.any_of().terms()) {
                auto reducedTerm = reduceRequirementTreeImpl(term, endorsements, message, snap);
                if (isEmptyTree(reducedTerm)) {
                    return rsp::proto::ERDAbstractSyntaxTree();
                }

                if (isFalseTree(reducedTerm)) {
                    continue;
                }

                remainingTerms.push_back(std::move(reducedTerm));
            }

            if (remainingTerms.empty()) {
                return makeFalseTree();
            }

            if (remainingTerms.size() == 1) {
                return remainingTerms.front();
            }

            return makeAnyOfTree(remainingTerms);
        }
        case rsp::proto::ERDAbstractSyntaxTree::kEndorsement:
        case rsp::proto::ERDAbstractSyntaxTree::kMessage:
        case rsp::proto::ERDAbstractSyntaxTree::kTrueValue:
        case rsp::proto::ERDAbstractSyntaxTree::kFalseValue:
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

Endorsement Endorsement::fromProto(const rsp::proto::Endorsement& message) {
    validateMessage(message, true);
    return Endorsement(message);
}

Endorsement Endorsement::deserialize(const Buffer& serialized) {
    rsp::proto::Endorsement message = deserializeBinary(serialized);
    validateMessage(message, true);
    return Endorsement(std::move(message));
}

Buffer Endorsement::serialize() const {
    validateMessage(message_, true);
    return serializeBinary(message_);
}

rsp::proto::Endorsement Endorsement::toProto() const {
    validateMessage(message_, true);
    return message_;
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

bool messageMatchesRequirement(const rsp::proto::ERDAbstractSyntaxTree& tree,
                              const rsp::proto::RSPMessage& message,
                              const rsp::resource_manager::SchemaSnapshot* schemaSnapshot) {
    try {
        return evaluateMessageRequirementTree(tree, message, schemaSnapshot);
    } catch (const std::runtime_error&) {
        return false;
    }
}

rsp::proto::ERDAbstractSyntaxTree reduceRequirementTree(const rsp::proto::ERDAbstractSyntaxTree& tree,
                                                        const std::vector<Endorsement>& endorsements,
                                                        const rsp::proto::RSPMessage* message) {
    return reduceRequirementTree(tree, endorsements, message, nullptr);
}

rsp::proto::ERDAbstractSyntaxTree reduceRequirementTree(const rsp::proto::ERDAbstractSyntaxTree& tree,
                                                        const std::vector<Endorsement>& endorsements,
                                                        const rsp::proto::RSPMessage* message,
                                                        const rsp::resource_manager::SchemaSnapshot* schemaSnapshot) {
    try {
        return reduceRequirementTreeImpl(tree, endorsements, message, schemaSnapshot);
    } catch (const std::runtime_error&) {
        return tree;
    }
}

}  // namespace rsp