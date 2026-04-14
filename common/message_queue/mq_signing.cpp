#include "common/message_queue/mq_signing.hpp"

#include <google/protobuf/any.pb.h>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor_database.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/message.h>
#include <openssl/sha.h>

#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

class MessageHasher {
    SHA256_CTX ctx_;

public:
    MessageHasher() {
        SHA256_Init(&ctx_);
    }

    void feed(const void* data, size_t length) {
        SHA256_Update(&ctx_, data, length);
    }

    void feedUint8(uint8_t value) {
        feed(&value, 1);
    }

    void feedUint32(uint32_t value) {
        const uint8_t bytes[4] = {
            static_cast<uint8_t>((value >> 24) & 0xFFU),
            static_cast<uint8_t>((value >> 16) & 0xFFU),
            static_cast<uint8_t>((value >> 8) & 0xFFU),
            static_cast<uint8_t>(value & 0xFFU),
        };
        feed(bytes, 4);
    }

    void feedInt32(int32_t value) {
        feedUint32(static_cast<uint32_t>(value));
    }

    void feedUint64(uint64_t value) {
        const uint8_t bytes[8] = {
            static_cast<uint8_t>((value >> 56) & 0xFFU),
            static_cast<uint8_t>((value >> 48) & 0xFFU),
            static_cast<uint8_t>((value >> 40) & 0xFFU),
            static_cast<uint8_t>((value >> 32) & 0xFFU),
            static_cast<uint8_t>((value >> 24) & 0xFFU),
            static_cast<uint8_t>((value >> 16) & 0xFFU),
            static_cast<uint8_t>((value >> 8) & 0xFFU),
            static_cast<uint8_t>(value & 0xFFU),
        };
        feed(bytes, 8);
    }

    void feedBool(bool value) {
        feedUint8(value ? 1 : 0);
    }

    void feedBytes(const std::string& value) {
        feedUint32(static_cast<uint32_t>(value.size()));
        feed(value.data(), value.size());
    }

    void tag(uint32_t fieldNumber) {
        feedUint32(fieldNumber);
    }

    rsp::MessageHash finalize() {
        rsp::MessageHash digest{};
        SHA256_Final(digest.data(), &ctx_);
        return digest;
    }
};

// Generic reflection-based hash: iterates fields sorted by number, matching the
// canonical hash produced by the hand-written functions and the JS/Python
// schema-based hash.  Used for service_message (google.protobuf.Any) payloads so
// that the RM can verify signatures without compile-time knowledge of service types.
void hashMessageReflective(MessageHasher& hasher, const google::protobuf::Message& message) {
    const auto* descriptor = message.GetDescriptor();
    const auto* reflection = message.GetReflection();
    if (descriptor == nullptr || reflection == nullptr) return;

    // Collect fields sorted by number (Descriptor::field() returns in declaration order
    // which SHOULD be by number, but sort to be safe).
    std::vector<const google::protobuf::FieldDescriptor*> fields;
    fields.reserve(static_cast<size_t>(descriptor->field_count()));
    for (int i = 0; i < descriptor->field_count(); i++) {
        fields.push_back(descriptor->field(i));
    }
    std::sort(fields.begin(), fields.end(),
              [](const auto* a, const auto* b) { return a->number() < b->number(); });

    for (const auto* field : fields) {
        if (field->is_repeated()) {
            const int count = reflection->FieldSize(message, field);
            hasher.tag(static_cast<uint32_t>(field->number()));
            hasher.feedUint32(static_cast<uint32_t>(count));
            for (int i = 0; i < count; i++) {
                switch (field->cpp_type()) {
                case google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE:
                    hashMessageReflective(hasher, reflection->GetRepeatedMessage(message, field, i));
                    break;
                case google::protobuf::FieldDescriptor::CPPTYPE_STRING:
                    hasher.feedBytes(reflection->GetRepeatedString(message, field, i));
                    break;
                case google::protobuf::FieldDescriptor::CPPTYPE_UINT32:
                    hasher.feedUint32(reflection->GetRepeatedUInt32(message, field, i));
                    break;
                case google::protobuf::FieldDescriptor::CPPTYPE_INT32:
                    hasher.feedInt32(reflection->GetRepeatedInt32(message, field, i));
                    break;
                case google::protobuf::FieldDescriptor::CPPTYPE_UINT64:
                    hasher.feedUint64(reflection->GetRepeatedUInt64(message, field, i));
                    break;
                case google::protobuf::FieldDescriptor::CPPTYPE_INT64:
                    hasher.feedUint64(static_cast<uint64_t>(reflection->GetRepeatedInt64(message, field, i)));
                    break;
                case google::protobuf::FieldDescriptor::CPPTYPE_BOOL:
                    hasher.feedBool(reflection->GetRepeatedBool(message, field, i));
                    break;
                case google::protobuf::FieldDescriptor::CPPTYPE_ENUM:
                    hasher.feedUint32(static_cast<uint32_t>(reflection->GetRepeatedEnumValue(message, field, i)));
                    break;
                default:
                    break;
                }
            }
            continue;
        }

        if (field->has_presence() && !reflection->HasField(message, field)) {
            continue;
        }

        hasher.tag(static_cast<uint32_t>(field->number()));
        switch (field->cpp_type()) {
        case google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE:
            hashMessageReflective(hasher, reflection->GetMessage(message, field));
            break;
        case google::protobuf::FieldDescriptor::CPPTYPE_STRING:
            hasher.feedBytes(reflection->GetString(message, field));
            break;
        case google::protobuf::FieldDescriptor::CPPTYPE_UINT32:
            hasher.feedUint32(reflection->GetUInt32(message, field));
            break;
        case google::protobuf::FieldDescriptor::CPPTYPE_INT32:
            hasher.feedInt32(reflection->GetInt32(message, field));
            break;
        case google::protobuf::FieldDescriptor::CPPTYPE_UINT64:
            hasher.feedUint64(reflection->GetUInt64(message, field));
            break;
        case google::protobuf::FieldDescriptor::CPPTYPE_INT64:
            hasher.feedUint64(static_cast<uint64_t>(reflection->GetInt64(message, field)));
            break;
        case google::protobuf::FieldDescriptor::CPPTYPE_BOOL:
            hasher.feedBool(reflection->GetBool(message, field));
            break;
        case google::protobuf::FieldDescriptor::CPPTYPE_ENUM:
            hasher.feedUint32(static_cast<uint32_t>(reflection->GetEnumValue(message, field)));
            break;
        default:
            break;
        }
    }
}

void hashNodeId(MessageHasher& hasher, const rsp::proto::NodeId& message) {
    hasher.tag(1);
    hasher.feedBytes(message.value());
}

void hashUuid(MessageHasher& hasher, const rsp::proto::Uuid& message) {
    hasher.tag(1);
    hasher.feedBytes(message.value());
}

void hashDateTime(MessageHasher& hasher, const rsp::proto::DateTime& message) {
    hasher.tag(1);
    hasher.feedUint64(message.milliseconds_since_epoch());
}

void hashPublicKey(MessageHasher& hasher, const rsp::proto::PublicKey& message) {
    hasher.tag(1);
    hasher.feedUint32(static_cast<uint32_t>(message.algorithm()));
    hasher.tag(2);
    hasher.feedBytes(message.public_key());
}

void hashEndorsement(MessageHasher& hasher, const rsp::proto::Endorsement& message) {
    if (message.has_subject()) {
        hasher.tag(1);
        hashNodeId(hasher, message.subject());
    }
    if (message.has_endorsement_service()) {
        hasher.tag(2);
        hashNodeId(hasher, message.endorsement_service());
    }
    if (message.has_endorsement_type()) {
        hasher.tag(3);
        hashUuid(hasher, message.endorsement_type());
    }
    hasher.tag(4);
    hasher.feedBytes(message.endorsement_value());
    if (message.has_valid_until()) {
        hasher.tag(5);
        hashDateTime(hasher, message.valid_until());
    }
    hasher.tag(99);
    hasher.feedBytes(message.signature());
}

void hashChallengeRequest(MessageHasher& hasher, const rsp::proto::ChallengeRequest& message) {
    if (message.has_nonce()) {
        hasher.tag(1);
        hashUuid(hasher, message.nonce());
    }
}

void hashIdentity(MessageHasher& hasher, const rsp::proto::Identity& message) {
    if (message.has_nonce()) {
        hasher.tag(1);
        hashUuid(hasher, message.nonce());
    }
    if (message.has_public_key()) {
        hasher.tag(2);
        hashPublicKey(hasher, message.public_key());
    }
}

void hashRouteEntry(MessageHasher& hasher, const rsp::proto::RouteEntry& message) {
    if (message.has_node_id()) {
        hasher.tag(1);
        hashNodeId(hasher, message.node_id());
    }
    hasher.tag(2);
    hasher.feedUint32(message.hops_away());
    if (message.has_last_seen()) {
        hasher.tag(3);
        hashDateTime(hasher, message.last_seen());
    }
}

void hashRouteUpdate(MessageHasher& hasher, const rsp::proto::RouteUpdate& message) {
    hasher.tag(2);
    hasher.feedUint32(static_cast<uint32_t>(message.entries_size()));
    for (int index = 0; index < message.entries_size(); ++index) {
        hashRouteEntry(hasher, message.entries(index));
    }
}

void hashError(MessageHasher& hasher, const rsp::proto::Error& message) {
    hasher.tag(1);
    hasher.feedUint32(static_cast<uint32_t>(message.error_code()));
    hasher.tag(2);
    hasher.feedBytes(message.message());
}

void hashPingRequest(MessageHasher& hasher, const rsp::proto::PingRequest& message) {
    if (message.has_nonce()) {
        hasher.tag(1);
        hashUuid(hasher, message.nonce());
    }
    hasher.tag(2);
    hasher.feedUint32(message.sequence());
    if (message.has_time_sent()) {
        hasher.tag(3);
        hashDateTime(hasher, message.time_sent());
    }
}

void hashPingReply(MessageHasher& hasher, const rsp::proto::PingReply& message) {
    if (message.has_nonce()) {
        hasher.tag(1);
        hashUuid(hasher, message.nonce());
    }
    hasher.tag(2);
    hasher.feedUint32(message.sequence());
    if (message.has_time_sent()) {
        hasher.tag(3);
        hashDateTime(hasher, message.time_sent());
    }
    if (message.has_time_replied()) {
        hasher.tag(4);
        hashDateTime(hasher, message.time_replied());
    }
}

void hashAddress(MessageHasher& hasher, const rsp::proto::Address& message) {
    hasher.tag(1);
    hasher.feedUint32(message.ipv4());
    hasher.tag(2);
    hasher.feedBytes(message.ipv6());
}

void hashPortRange(MessageHasher& hasher, const rsp::proto::PortRange& message) {
    hasher.tag(1);
    hasher.feedUint32(message.start_port());
    hasher.tag(2);
    hasher.feedUint32(message.end_port());
}

void hashResourceTcpConnect(MessageHasher& hasher, const rsp::proto::ResourceTCPConnect& message) {
    if (message.has_node_id()) {
        hasher.tag(1);
        hashNodeId(hasher, message.node_id());
    }
    hasher.tag(2);
    hasher.feedUint32(static_cast<uint32_t>(message.source_addresses_size()));
    for (int index = 0; index < message.source_addresses_size(); ++index) {
        hashAddress(hasher, message.source_addresses(index));
    }
}

void hashResourceTcpListen(MessageHasher& hasher, const rsp::proto::ResourceTCPListen& message) {
    if (message.has_node_id()) {
        hasher.tag(1);
        hashNodeId(hasher, message.node_id());
    }
    hasher.tag(2);
    hasher.feedUint32(static_cast<uint32_t>(message.listen_address_size()));
    for (int index = 0; index < message.listen_address_size(); ++index) {
        hashAddress(hasher, message.listen_address(index));
    }
    if (message.has_allowed_range()) {
        hasher.tag(3);
        hashPortRange(hasher, message.allowed_range());
    }
}

void hashResourceRecord(MessageHasher& hasher, const rsp::proto::ResourceRecord& message) {
    switch (message.resource_type_case()) {
    case rsp::proto::ResourceRecord::kTcpConnect:
        hasher.tag(1);
        hashResourceTcpConnect(hasher, message.tcp_connect());
        break;
    case rsp::proto::ResourceRecord::kTcpListen:
        hasher.tag(2);
        hashResourceTcpListen(hasher, message.tcp_listen());
        break;
    case rsp::proto::ResourceRecord::RESOURCE_TYPE_NOT_SET:
        break;
    }
}

void hashResourceAdvertisement(MessageHasher& hasher, const rsp::proto::ResourceAdvertisement& message) {
    hasher.tag(1);
    hasher.feedUint32(static_cast<uint32_t>(message.records_size()));
    for (int index = 0; index < message.records_size(); ++index) {
        hashResourceRecord(hasher, message.records(index));
    }
    if (message.schemas_size() > 0) {
        hasher.tag(2);
        hasher.feedUint32(static_cast<uint32_t>(message.schemas_size()));
        for (int index = 0; index < message.schemas_size(); ++index) {
            const auto& schema = message.schemas(index);
            hasher.feedBytes(schema.proto_file_name());
            hasher.feedBytes(schema.proto_file_descriptor_set());
            hasher.feedUint32(static_cast<uint32_t>(schema.accepted_type_urls_size()));
            for (int urlIndex = 0; urlIndex < schema.accepted_type_urls_size(); ++urlIndex) {
                hasher.feedBytes(schema.accepted_type_urls(urlIndex));
            }
        }
    }
}

void hashResourceQuery(MessageHasher& hasher, const rsp::proto::ResourceQuery& message) {
    hasher.tag(1);
    hasher.feedBytes(message.query());
    hasher.tag(2);
    hasher.feedUint32(message.max_records());
}

void hashEndorsementNeeded(MessageHasher& hasher, const rsp::proto::EndorsementNeeded& message) {
    if (message.has_message_nonce()) {
        hasher.tag(1);
        hashUuid(hasher, message.message_nonce());
    }
    if (message.has_tree()) {
        std::string serializedTree;
        if (!message.tree().SerializeToString(&serializedTree)) {
            throw std::runtime_error("failed to serialize endorsement requirement tree");
        }
        hasher.tag(2);
        hasher.feedBytes(serializedTree);
    }
}

void hashRSPMessage(MessageHasher& hasher, const rsp::proto::RSPMessage& message) {
    if (message.has_destination()) {
        hasher.tag(1);
        hashNodeId(hasher, message.destination());
    }

    if (message.has_source()) {
        hasher.tag(2);
        hashNodeId(hasher, message.source());
    }

    if (message.has_trace()) {
        hasher.tag(24);
        hasher.feedBool(message.trace().value());
    }

    switch (message.core_message_case()) {
    case rsp::proto::RSPMessage::kChallengeRequest:
        hasher.tag(3);
        hashChallengeRequest(hasher, message.challenge_request());
        break;
    case rsp::proto::RSPMessage::kIdentity:
        hasher.tag(4);
        hashIdentity(hasher, message.identity());
        break;
    case rsp::proto::RSPMessage::kRoute:
        hasher.tag(5);
        hashRouteUpdate(hasher, message.route());
        break;
    case rsp::proto::RSPMessage::kError:
        hasher.tag(6);
        hashError(hasher, message.error());
        break;
    case rsp::proto::RSPMessage::kPingRequest:
        hasher.tag(7);
        hashPingRequest(hasher, message.ping_request());
        break;
    case rsp::proto::RSPMessage::kPingReply:
        hasher.tag(8);
        hashPingReply(hasher, message.ping_reply());
        break;
    case rsp::proto::RSPMessage::kResourceAdvertisement:
        hasher.tag(16);
        hashResourceAdvertisement(hasher, message.resource_advertisement());
        break;
    case rsp::proto::RSPMessage::kResourceQuery:
        hasher.tag(17);
        hashResourceQuery(hasher, message.resource_query());
        break;
    case rsp::proto::RSPMessage::kEndorsementNeeded:
        hasher.tag(23);
        hashEndorsementNeeded(hasher, message.endorsement_needed());
        break;
    case rsp::proto::RSPMessage::CORE_MESSAGE_NOT_SET:
        break;
    }

    // Service-specific payload (google.protobuf.Any): hash type_url then hash
    // the inner message field-by-field using reflection so the result matches
    // the canonical hash the JS/Python clients produce from their JSON schemas.
    if (message.has_service_message()) {
        hasher.tag(200);
        const auto& any = message.service_message();
        hasher.feedBytes(any.type_url());

        // Resolve the concrete message type from the linked-in descriptor pool
        const std::string& typeUrl = any.type_url();
        const auto slashPos = typeUrl.rfind('/');
        const std::string fullName = slashPos != std::string::npos
                                         ? typeUrl.substr(slashPos + 1)
                                         : typeUrl;
        const auto* descriptor = google::protobuf::DescriptorPool::generated_pool()
                                     ->FindMessageTypeByName(fullName);
        if (descriptor != nullptr) {
            const auto* prototype = google::protobuf::MessageFactory::generated_factory()
                                        ->GetPrototype(descriptor);
            if (prototype != nullptr) {
                std::unique_ptr<google::protobuf::Message> inner(prototype->New());
                if (any.UnpackTo(inner.get())) {
                    hashMessageReflective(hasher, *inner);
                }
            }
        }
    }

    if (message.has_nonce()) {
        hasher.tag(22);
        hashUuid(hasher, message.nonce());
    }

    hasher.tag(100);
    hasher.feedUint32(static_cast<uint32_t>(message.endorsements_size()));
    for (int index = 0; index < message.endorsements_size(); ++index) {
        hashEndorsement(hasher, message.endorsements(index));
    }
}

}  // namespace

rsp::MessageHash rsp::computeMessageHash(const rsp::proto::RSPMessage& message) {
    MessageHasher hasher;
    hashRSPMessage(hasher, message);
    return hasher.finalize();
}

rsp::Buffer rsp::messageSignatureInput(const rsp::proto::RSPMessage& message) {
    const MessageHash hash = computeMessageHash(message);
    return Buffer(hash.data(), static_cast<uint32_t>(hash.size()));
}

rsp::proto::SignatureBlock rsp::signMessage(const KeyPair& keyPair, const rsp::proto::RSPMessage& message) {
    return keyPair.signBlock(messageSignatureInput(message));
}

bool rsp::verifyMessageSignature(const KeyPair& keyPair,
                                 const rsp::proto::RSPMessage& message,
                                 const rsp::proto::SignatureBlock& signatureBlock) {
    return keyPair.verifyBlock(messageSignatureInput(message), signatureBlock);
}

// Extracts a NodeID from a source/destination NodeId proto field.
// These fields use native-endian memcpy encoding (matching rsp_client.cpp).
std::optional<rsp::NodeID> rsp::nodeIdFromSourceField(const rsp::proto::NodeId& protoId) {
    if (protoId.value().size() != 16) {
        return std::nullopt;
    }

    uint64_t high = 0;
    uint64_t low = 0;
    std::memcpy(&high, protoId.value().data(), sizeof(high));
    std::memcpy(&low, protoId.value().data() + sizeof(high), sizeof(low));
    return rsp::NodeID(high, low);
}

// Extracts a NodeID from a SignatureBlock signer NodeId proto field.
// These fields use big-endian byte-shift encoding (matching keypair.cpp signBlock).
std::optional<rsp::NodeID> rsp::nodeIdFromSignerField(const rsp::proto::NodeId& protoId) {
    const std::string& bytes = protoId.value();
    if (bytes.size() != 16) {
        return std::nullopt;
    }

    uint64_t high = 0;
    uint64_t low = 0;
    for (int i = 0; i < 8; ++i) {
        high = (high << 8) | static_cast<uint64_t>(static_cast<unsigned char>(bytes[i]));
    }
    for (int i = 8; i < 16; ++i) {
        low = (low << 8) | static_cast<uint64_t>(static_cast<unsigned char>(bytes[i]));
    }
    return rsp::NodeID(high, low);
}

std::optional<rsp::NodeID> rsp::senderNodeIdFromMessage(const rsp::proto::RSPMessage& message) {
    if (message.has_signature() && message.signature().has_signer()) {
        const auto signerNodeId = nodeIdFromSignerField(message.signature().signer());
        if (signerNodeId.has_value()) {
            return signerNodeId;
        }
    }

    if (message.has_source()) {
        return nodeIdFromSourceField(message.source());
    }

    return std::nullopt;
}

bool rsp::messageTraceEnabled(const rsp::proto::RSPMessage& message) {
    return message.has_trace() && message.trace().value();
}

void rsp::copyMessageTrace(const rsp::proto::RSPMessage& source, rsp::proto::RSPMessage& destination) {
    if (source.has_trace()) {
        destination.mutable_trace()->set_value(source.trace().value());
    }
}

namespace rsp {

//
// MessageQueueSign
//

MessageQueueSign::MessageQueueSign(std::function<void(rsp::proto::RSPMessage)> onSuccess,
                                   std::function<void(rsp::proto::RSPMessage, std::string)> onFailure,
                                   GetKeyFunction getKeyForNodeID)
    : onSuccess_(std::move(onSuccess))
    , onFailure_(std::move(onFailure))
    , getKey_(std::move(getKeyForNodeID)) {
}

MessageQueueSign::MessageQueueSign(std::function<void(rsp::proto::RSPMessage)> onSuccess,
                                   std::function<void(rsp::proto::RSPMessage, std::string)> onFailure,
                                   KeyPair keyPair)
    : onSuccess_(std::move(onSuccess))
    , onFailure_(std::move(onFailure)) {
    auto sharedKey = std::make_shared<KeyPair>(std::move(keyPair));
    getKey_ = [sharedKey](const NodeID&) -> std::shared_ptr<const KeyPair> {
        return sharedKey;
    };
}

void MessageQueueSign::handleMessage(Message message, MessageQueueSharedState&) {
    const bool trace = messageTraceEnabled(message);
    if (trace) {
        std::cerr << "[mq_sign] entry" << std::endl;
    }
    const auto requestedNodeId = senderNodeIdFromMessage(message).value_or(NodeID());
    const auto keyPair = getKey_(requestedNodeId);
    if (!keyPair) {
        if (trace) {
            std::cerr << "[mq_sign] failure: no signing key for sender" << std::endl;
        }
        onFailure_(std::move(message), "no signing key found for message sender");
        return;
    }

    if (!keyPair->hasPrivateKey()) {
        if (trace) {
            std::cerr << "[mq_sign] failure: signer key has no private key" << std::endl;
        }
        onFailure_(std::move(message), "key for message sender has no private key");
        return;
    }

    try {
        *message.mutable_signature() = signMessage(*keyPair, message);
        if (trace) {
            std::cerr << "[mq_sign] success" << std::endl;
        }
        onSuccess_(std::move(message));
    } catch (const std::exception& e) {
        if (trace) {
            std::cerr << "[mq_sign] failure: signing exception " << e.what() << std::endl;
        }
        onFailure_(std::move(message), std::string("signing failed: ") + e.what());
    }
}

void MessageQueueSign::handleQueueFull(size_t, size_t limit, const Message& rejected) {
    if (messageTraceEnabled(rejected)) {
        std::cerr << "[mq_sign] queue_full limit=" << limit << std::endl;
    }
    if (onFailure_) {
        onFailure_(rejected, "signing queue full (limit=" + std::to_string(limit) + ")");
    }
}

//
// MessageQueueCheckSignature
//

MessageQueueCheckSignature::MessageQueueCheckSignature(
    std::function<void(rsp::proto::RSPMessage)> onSuccess,
    std::function<void(rsp::proto::RSPMessage, std::string)> onFailure,
    GetKeyFunction getKeyForNodeID)
    : onSuccess_(std::move(onSuccess))
    , onFailure_(std::move(onFailure))
    , getKey_(std::move(getKeyForNodeID)) {
}

MessageQueueCheckSignature::MessageQueueCheckSignature(
    std::function<void(rsp::proto::RSPMessage)> onSuccess,
    std::function<void(rsp::proto::RSPMessage, std::string)> onFailure,
    KeyPair keyPair)
    : onSuccess_(std::move(onSuccess))
    , onFailure_(std::move(onFailure)) {
    auto sharedKey = std::make_shared<KeyPair>(std::move(keyPair));
    getKey_ = [sharedKey](const NodeID& nodeId) -> std::shared_ptr<const KeyPair> {
        if (nodeId == sharedKey->nodeID()) {
            return sharedKey;
        }
        return nullptr;
    };
}

void MessageQueueCheckSignature::handleMessage(Message message, MessageQueueSharedState&) {
    const bool trace = messageTraceEnabled(message);
    if (trace) {
        std::cerr << "[mq_check_signature] entry" << std::endl;
    }
    if (!message.has_signature()) {
        if (trace) {
            std::cerr << "[mq_signing] reject: message has no signature" << std::endl;
        }
        onFailure_(std::move(message), "message has no signature");
        return;
    }

    const auto nodeId = nodeIdFromSignerField(message.signature().signer());
    if (!nodeId.has_value()) {
        if (trace) {
            std::cerr << "[mq_signing] reject: signature signer has invalid node ID encoding" << std::endl;
        }
        onFailure_(std::move(message), "signature signer has invalid node ID encoding");
        return;
    }

    const auto keyPair = getKey_(*nodeId);
    if (!keyPair) {
        if (trace) {
            std::cerr << "[mq_signing] reject: no verification key for signer " << nodeId->toString() << std::endl;
        }
        onFailure_(std::move(message), "no verification key found for signer node ID");
        return;
    }

    try {
        if (verifyMessageSignature(*keyPair, message, message.signature())) {
            if (trace) {
                std::cerr << "[mq_check_signature] success" << std::endl;
            }
            onSuccess_(std::move(message));
        } else {
            if (trace) {
                std::cerr << "[mq_signing] reject: signature verification failed for signer "
                          << nodeId->toString() << std::endl;
            }
            onFailure_(std::move(message), "signature verification failed");
        }
    } catch (const std::exception& e) {
        if (trace) {
            std::cerr << "[mq_signing] reject: exception during signature check: " << e.what() << std::endl;
        }
        onFailure_(std::move(message), std::string("signature check failed: ") + e.what());
    }
}

void MessageQueueCheckSignature::handleQueueFull(size_t, size_t limit, const Message& rejected) {
    if (messageTraceEnabled(rejected)) {
        std::cerr << "[mq_check_signature] queue_full limit=" << limit << std::endl;
    }
    if (onFailure_) {
        onFailure_(rejected, "check-signature queue full (limit=" + std::to_string(limit) + ")");
    }
}

}  // namespace rsp
