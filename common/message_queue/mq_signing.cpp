#include "common/message_queue/mq_signing.hpp"

#include <openssl/sha.h>

#include <array>
#include <cstring>
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

void hashNodeId(MessageHasher& hasher, const rsp::proto::NodeId& message) {
    hasher.tag(1);
    hasher.feedBytes(message.value());
}

void hashSocketId(MessageHasher& hasher, const rsp::proto::SocketID& message) {
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
}

void hashResourceQuery(MessageHasher& hasher, const rsp::proto::ResourceQuery& message) {
    hasher.tag(1);
    hasher.feedBytes(message.query());
    hasher.tag(2);
    hasher.feedUint32(message.max_records());
}

void hashSocketReply(MessageHasher& hasher, const rsp::proto::SocketReply& message) {
    if (message.has_socket_id()) {
        hasher.tag(1);
        hashSocketId(hasher, message.socket_id());
    }
    hasher.tag(2);
    hasher.feedUint32(static_cast<uint32_t>(message.error()));
    if (message.has_message()) {
        hasher.tag(3);
        hasher.feedBytes(message.message());
    }
    if (message.has_new_socket_remote_address()) {
        hasher.tag(4);
        hasher.feedBytes(message.new_socket_remote_address());
    }
    if (message.has_new_socket_id()) {
        hasher.tag(5);
        hashSocketId(hasher, message.new_socket_id());
    }
    if (message.has_socket_error_code()) {
        hasher.tag(6);
        hasher.feedInt32(message.socket_error_code());
    }
    if (message.has_data()) {
        hasher.tag(7);
        hasher.feedBytes(message.data());
    }
}

void hashConnectTcpRequest(MessageHasher& hasher, const rsp::proto::ConnectTCPRequest& message) {
    hasher.tag(1);
    hasher.feedBytes(message.host_port());
    if (message.has_socket_number()) {
        hasher.tag(2);
        hashSocketId(hasher, message.socket_number());
    }
    if (message.has_reuse_addr()) {
        hasher.tag(3);
        hasher.feedBool(message.reuse_addr());
    }
    if (message.has_source_port()) {
        hasher.tag(4);
        hasher.feedUint32(message.source_port());
    }
    if (message.has_timeout_ms()) {
        hasher.tag(5);
        hasher.feedUint32(message.timeout_ms());
    }
    if (message.has_retries()) {
        hasher.tag(6);
        hasher.feedUint32(message.retries());
    }
    if (message.has_retry_ms()) {
        hasher.tag(7);
        hasher.feedUint32(message.retry_ms());
    }
    if (message.has_async_data()) {
        hasher.tag(8);
        hasher.feedBool(message.async_data());
    }
    if (message.has_use_socket()) {
        hasher.tag(9);
        hasher.feedBool(message.use_socket());
    }
    if (message.has_share_socket()) {
        hasher.tag(10);
        hasher.feedBool(message.share_socket());
    }
}

void hashListenTcpRequest(MessageHasher& hasher, const rsp::proto::ListenTCPRequest& message) {
    hasher.tag(1);
    hasher.feedBytes(message.host_port());
    if (message.has_socket_number()) {
        hasher.tag(2);
        hashSocketId(hasher, message.socket_number());
    }
    if (message.has_reuse_addr()) {
        hasher.tag(3);
        hasher.feedBool(message.reuse_addr());
    }
    if (message.has_timeout_ms()) {
        hasher.tag(4);
        hasher.feedUint32(message.timeout_ms());
    }
    if (message.has_async_accept()) {
        hasher.tag(5);
        hasher.feedBool(message.async_accept());
    }
    if (message.has_share_listening_socket()) {
        hasher.tag(6);
        hasher.feedBool(message.share_listening_socket());
    }
    if (message.has_share_child_sockets()) {
        hasher.tag(7);
        hasher.feedBool(message.share_child_sockets());
    }
    if (message.has_children_use_socket()) {
        hasher.tag(8);
        hasher.feedBool(message.children_use_socket());
    }
    if (message.has_children_async_data()) {
        hasher.tag(9);
        hasher.feedBool(message.children_async_data());
    }
}

void hashAcceptTcp(MessageHasher& hasher, const rsp::proto::AcceptTCP& message) {
    if (message.has_listen_socket_number()) {
        hasher.tag(1);
        hashSocketId(hasher, message.listen_socket_number());
    }
    if (message.has_new_socket_number()) {
        hasher.tag(2);
        hashSocketId(hasher, message.new_socket_number());
    }
    if (message.has_timeout_ms()) {
        hasher.tag(3);
        hasher.feedUint32(message.timeout_ms());
    }
    if (message.has_share_child_socket()) {
        hasher.tag(4);
        hasher.feedBool(message.share_child_socket());
    }
    if (message.has_child_use_socket()) {
        hasher.tag(5);
        hasher.feedBool(message.child_use_socket());
    }
    if (message.has_child_async_data()) {
        hasher.tag(6);
        hasher.feedBool(message.child_async_data());
    }
}

void hashSocketSend(MessageHasher& hasher, const rsp::proto::SocketSend& message) {
    if (message.has_socket_number()) {
        hasher.tag(1);
        hashSocketId(hasher, message.socket_number());
    }
    hasher.tag(2);
    hasher.feedBytes(message.data());
    hasher.tag(3);
    hasher.feedUint64(message.index());
}

void hashSocketRecv(MessageHasher& hasher, const rsp::proto::SocketRecv& message) {
    if (message.has_socket_number()) {
        hasher.tag(1);
        hashSocketId(hasher, message.socket_number());
    }
    if (message.has_max_bytes()) {
        hasher.tag(2);
        hasher.feedUint32(message.max_bytes());
    }
    hasher.tag(3);
    hasher.feedUint64(message.index());
    if (message.has_wait_ms()) {
        hasher.tag(4);
        hasher.feedUint32(message.wait_ms());
    }
}

void hashSocketClose(MessageHasher& hasher, const rsp::proto::SocketClose& message) {
    if (message.has_socket_number()) {
        hasher.tag(1);
        hashSocketId(hasher, message.socket_number());
    }
}

void hashBeginEndorsementRequest(MessageHasher& hasher, const rsp::proto::BeginEndorsementRequest& message) {
    if (message.has_requested_values()) {
        hasher.tag(1);
        hashEndorsement(hasher, message.requested_values());
    }
    if (message.has_auth_data()) {
        hasher.tag(2);
        hasher.feedBytes(message.auth_data());
    }
}

void hashEndorsementChallenge(MessageHasher& hasher, const rsp::proto::EndorsementChallenge& message) {
    if (message.has_stage()) {
        hasher.tag(1);
        hasher.feedUint32(message.stage());
    }
    if (message.has_challenge()) {
        hasher.tag(2);
        hasher.feedBytes(message.challenge());
    }
}

void hashEndorsementChallengeReply(MessageHasher& hasher, const rsp::proto::EndorsementChallengeReply& message) {
    if (message.has_stage()) {
        hasher.tag(1);
        hasher.feedUint32(message.stage());
    }
    if (message.has_challenge_reply()) {
        hasher.tag(2);
        hasher.feedBytes(message.challenge_reply());
    }
}

void hashEndorsementDone(MessageHasher& hasher, const rsp::proto::EndorsementDone& message) {
    hasher.tag(1);
    hasher.feedUint32(static_cast<uint32_t>(message.status()));
    if (message.has_new_endorsement()) {
        hasher.tag(2);
        hashEndorsement(hasher, message.new_endorsement());
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

    switch (message.submessage_case()) {
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
    case rsp::proto::RSPMessage::kConnectTcpRequest:
        hasher.tag(9);
        hashConnectTcpRequest(hasher, message.connect_tcp_request());
        break;
    case rsp::proto::RSPMessage::kSocketReply:
        hasher.tag(10);
        hashSocketReply(hasher, message.socket_reply());
        break;
    case rsp::proto::RSPMessage::kSocketSend:
        hasher.tag(11);
        hashSocketSend(hasher, message.socket_send());
        break;
    case rsp::proto::RSPMessage::kSocketRecv:
        hasher.tag(12);
        hashSocketRecv(hasher, message.socket_recv());
        break;
    case rsp::proto::RSPMessage::kSocketClose:
        hasher.tag(13);
        hashSocketClose(hasher, message.socket_close());
        break;
    case rsp::proto::RSPMessage::kListenTcpRequest:
        hasher.tag(14);
        hashListenTcpRequest(hasher, message.listen_tcp_request());
        break;
    case rsp::proto::RSPMessage::kAcceptTcp:
        hasher.tag(15);
        hashAcceptTcp(hasher, message.accept_tcp());
        break;
    case rsp::proto::RSPMessage::kResourceAdvertisement:
        hasher.tag(16);
        hashResourceAdvertisement(hasher, message.resource_advertisement());
        break;
    case rsp::proto::RSPMessage::kResourceQuery:
        hasher.tag(17);
        hashResourceQuery(hasher, message.resource_query());
        break;
    case rsp::proto::RSPMessage::kBeginEndorsementRequest:
        hasher.tag(18);
        hashBeginEndorsementRequest(hasher, message.begin_endorsement_request());
        break;
    case rsp::proto::RSPMessage::kEndorsementChallenge:
        hasher.tag(19);
        hashEndorsementChallenge(hasher, message.endorsement_challenge());
        break;
    case rsp::proto::RSPMessage::kEndorsementChallengeReply:
        hasher.tag(20);
        hashEndorsementChallengeReply(hasher, message.endorsement_challenge_reply());
        break;
    case rsp::proto::RSPMessage::kEndorsementDone:
        hasher.tag(21);
        hashEndorsementDone(hasher, message.endorsement_done());
        break;
    case rsp::proto::RSPMessage::SUBMESSAGE_NOT_SET:
        break;
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
        *message.mutable_signature() = signMessage(*keyPair, message);
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
        if (verifyMessageSignature(*keyPair, message, message.signature())) {
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
