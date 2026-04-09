#include "common/message_queue/mq_signing.hpp"

#include <cstring>
#include <stdexcept>
#include <string>

// Serializes the message with the signature field cleared, for signing/verification.
rsp::Buffer rsp::serializeMessageForSigning(const rsp::proto::RSPMessage& message) {
    rsp::proto::RSPMessage copy = message;
    copy.clear_signature();

    std::string payload;
    if (!copy.SerializeToString(&payload)) {
        return rsp::Buffer();
    }

    if (payload.empty()) {
        return rsp::Buffer();
    }

    return rsp::Buffer(reinterpret_cast<const uint8_t*>(payload.data()),
                       static_cast<uint32_t>(payload.size()));
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
    const auto requestedNodeId = senderNodeIdFromMessage(message).value_or(NodeID());
    const auto keyPair = getKey_(requestedNodeId);
    if (!keyPair) {
        onFailure_(std::move(message), "no signing key found for message sender");
        return;
    }

    if (!keyPair->hasPrivateKey()) {
        onFailure_(std::move(message), "key for message sender has no private key");
        return;
    }

    try {
        const Buffer serialized = serializeMessageForSigning(message);
        *message.mutable_signature() = keyPair->signBlock(serialized);
        onSuccess_(std::move(message));
    } catch (const std::exception& e) {
        onFailure_(std::move(message), std::string("signing failed: ") + e.what());
    }
}

void MessageQueueSign::handleQueueFull(size_t, size_t limit, const Message& rejected) {
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
    if (!message.has_signature()) {
        onFailure_(std::move(message), "message has no signature");
        return;
    }

    const auto nodeId = nodeIdFromSignerField(message.signature().signer());
    if (!nodeId.has_value()) {
        onFailure_(std::move(message), "signature signer has invalid node ID encoding");
        return;
    }

    const auto keyPair = getKey_(*nodeId);
    if (!keyPair) {
        onFailure_(std::move(message), "no verification key found for signer node ID");
        return;
    }

    try {
        const Buffer serialized = serializeMessageForSigning(message);
        if (keyPair->verifyBlock(serialized, message.signature())) {
            onSuccess_(std::move(message));
        } else {
            onFailure_(std::move(message), "signature verification failed");
        }
    } catch (const std::exception& e) {
        onFailure_(std::move(message), std::string("signature check failed: ") + e.what());
    }
}

void MessageQueueCheckSignature::handleQueueFull(size_t, size_t limit, const Message& rejected) {
    if (onFailure_) {
        onFailure_(rejected, "check-signature queue full (limit=" + std::to_string(limit) + ")");
    }
}

}  // namespace rsp
