#include "resource_manager/resource_manager.hpp"

#include "common/message_queue/mq_ascii_handshake.hpp"
#include "common/message_queue/mq_authn.hpp"
#include "common/message_queue/mq_authz.hpp"
#include "common/message_queue/mq_signing.hpp"
#include "common/ping_trace.hpp"
#include "os/os_random.hpp"

#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace rsp::resource_manager {

namespace {

rsp::proto::NodeId toProtoNodeId(const rsp::NodeID& nodeId) {
    rsp::proto::NodeId protoNodeId;
    std::string value(16, '\0');
    const uint64_t high = nodeId.high();
    const uint64_t low = nodeId.low();
    std::memcpy(value.data(), &high, sizeof(high));
    std::memcpy(value.data() + sizeof(high), &low, sizeof(low));
    protoNodeId.set_value(value);
    return protoNodeId;
}

std::optional<rsp::NodeID> fromProtoNodeId(const rsp::proto::NodeId& nodeId) {
    if (nodeId.value().size() != 16) {
        return std::nullopt;
    }

    uint64_t high = 0;
    uint64_t low = 0;
    std::memcpy(&high, nodeId.value().data(), sizeof(high));
    std::memcpy(&low, nodeId.value().data() + sizeof(high), sizeof(low));
    return rsp::NodeID(high, low);
}

void setNodeIdIfPresent(rsp::proto::ResourceRecord* record, const rsp::NodeID& nodeId) {
    if (record == nullptr) {
        return;
    }

    const auto protoNodeId = toProtoNodeId(nodeId);
    if (record->has_tcp_connect()) {
        *record->mutable_tcp_connect()->mutable_node_id() = protoNodeId;
        return;
    }

    if (record->has_tcp_listen()) {
        *record->mutable_tcp_listen()->mutable_node_id() = protoNodeId;
    }
}

std::string randomMessageNonce() {
    std::string nonce(16, '\0');
    rsp::os::randomFill(reinterpret_cast<uint8_t*>(nonce.data()), static_cast<uint32_t>(nonce.size()));
    return nonce;
}

class IncomingMessageQueue : public rsp::RSPMessageQueue {
public:
    explicit IncomingMessageQueue(ResourceManager& owner) : owner_(owner) {
    }

protected:
    void handleMessage(Message message, rsp::MessageQueueSharedState&) override {
        if (owner_.isForThisNode(message)) {
            if (!owner_.enqueueInput(std::move(message))) {
                std::cerr << "ResourceManager failed to enqueue local message on input queue" << std::endl;
            }
            return;
        }

        owner_.observeMessage(message);

        if (!owner_.routeAndSend(message)) {
            std::cerr << "ResourceManager failed to route incoming message" << std::endl;
        }
    }

    void handleQueueFull(size_t, size_t, const Message&) override {
        std::cerr << "ResourceManager incoming message queue dropped message because the queue is full" << std::endl;
    }

private:
    ResourceManager& owner_;
};

}  // namespace

ResourceManager::ResourceManager()
    : incomingMessages_(std::make_shared<IncomingMessageQueue>(*this)),
      authzQueue_(nullptr),
      signatureCheckQueue_(std::make_shared<rsp::MessageQueueCheckSignature>(
          [this](rsp::proto::RSPMessage message) { handleVerifiedMessage(std::move(message)); },
          [this](rsp::proto::RSPMessage message, std::string reason) {
              handleSignatureFailure(std::move(message), reason);
          },
          [this](const rsp::NodeID& nodeId) { return verificationKeyForNodeId(nodeId); })),
      handshakeQueue_(std::make_shared<rsp::message_queue::MessageQueueAsciiHandshakeServer>(
          signatureCheckQueue_,
          keyPair().duplicate(),
          [this](const rsp::encoding::EncodingHandle& encoding) { handleHandshakeSuccess(encoding); },
          [](const rsp::transport::ConnectionHandle& connection) {
              if (connection != nullptr) {
                  connection->close();
              }
          })),
      authnQueue_(std::make_shared<rsp::message_queue::MessageQueueAuthN>(
          keyPair().duplicate(),
          [this](const rsp::encoding::EncodingHandle& encoding) { handleAuthNSuccess(encoding); },
          [this](const rsp::encoding::EncodingHandle& encoding) { handleAuthNFailure(encoding); },
          [this](const rsp::NodeID& peerNodeId, const rsp::proto::Identity& identity) {
              cacheAuthenticatedIdentity(peerNodeId, identity);
          })) {
    incomingMessages_->setWorkerCount(1);
    incomingMessages_->start();
        rebuildAuthorizationQueue();
    signatureCheckQueue_->setWorkerCount(1);
    signatureCheckQueue_->start();
    handshakeQueue_->setWorkerCount(4);
    handshakeQueue_->start();
    authnQueue_->setWorkerCount(4);
    authnQueue_->start();
    registerTransportCallbacks();
}

ResourceManager::ResourceManager(std::vector<rsp::transport::ListeningTransportHandle> clientTransports)
    : incomingMessages_(std::make_shared<IncomingMessageQueue>(*this)),
      authzQueue_(nullptr),
      signatureCheckQueue_(std::make_shared<rsp::MessageQueueCheckSignature>(
          [this](rsp::proto::RSPMessage message) { handleVerifiedMessage(std::move(message)); },
          [this](rsp::proto::RSPMessage message, std::string reason) {
              handleSignatureFailure(std::move(message), reason);
          },
          [this](const rsp::NodeID& nodeId) { return verificationKeyForNodeId(nodeId); })),
      handshakeQueue_(std::make_shared<rsp::message_queue::MessageQueueAsciiHandshakeServer>(
          signatureCheckQueue_,
          keyPair().duplicate(),
          [this](const rsp::encoding::EncodingHandle& encoding) { handleHandshakeSuccess(encoding); },
          [](const rsp::transport::ConnectionHandle& connection) {
              if (connection != nullptr) {
                  connection->close();
              }
          })),
      authnQueue_(std::make_shared<rsp::message_queue::MessageQueueAuthN>(
          keyPair().duplicate(),
          [this](const rsp::encoding::EncodingHandle& encoding) { handleAuthNSuccess(encoding); },
          [this](const rsp::encoding::EncodingHandle& encoding) { handleAuthNFailure(encoding); },
          [this](const rsp::NodeID& peerNodeId, const rsp::proto::Identity& identity) {
              cacheAuthenticatedIdentity(peerNodeId, identity);
          })),
      clientTransports_(std::move(clientTransports)) {
    incomingMessages_->setWorkerCount(1);
    incomingMessages_->start();
        rebuildAuthorizationQueue();
    signatureCheckQueue_->setWorkerCount(1);
    signatureCheckQueue_->start();
    handshakeQueue_->setWorkerCount(4);
    handshakeQueue_->start();
    authnQueue_->setWorkerCount(4);
    authnQueue_->start();
    registerTransportCallbacks();
}

ResourceManager::ResourceManager(rsp::KeyPair keyPair,
                                 std::vector<rsp::transport::ListeningTransportHandle> clientTransports)
    : RSPNode(std::move(keyPair)),
      incomingMessages_(std::make_shared<IncomingMessageQueue>(*this)),
      authzQueue_(nullptr),
      signatureCheckQueue_(std::make_shared<rsp::MessageQueueCheckSignature>(
          [this](rsp::proto::RSPMessage message) { handleVerifiedMessage(std::move(message)); },
          [this](rsp::proto::RSPMessage message, std::string reason) {
              handleSignatureFailure(std::move(message), reason);
          },
          [this](const rsp::NodeID& nodeId) { return verificationKeyForNodeId(nodeId); })),
      handshakeQueue_(std::make_shared<rsp::message_queue::MessageQueueAsciiHandshakeServer>(
          signatureCheckQueue_,
          this->keyPair().duplicate(),
          [this](const rsp::encoding::EncodingHandle& encoding) { handleHandshakeSuccess(encoding); },
          [](const rsp::transport::ConnectionHandle& connection) {
              if (connection != nullptr) {
                  connection->close();
              }
          })),
      authnQueue_(std::make_shared<rsp::message_queue::MessageQueueAuthN>(
          this->keyPair().duplicate(),
          [this](const rsp::encoding::EncodingHandle& encoding) { handleAuthNSuccess(encoding); },
          [this](const rsp::encoding::EncodingHandle& encoding) { handleAuthNFailure(encoding); },
          [this](const rsp::NodeID& peerNodeId, const rsp::proto::Identity& identity) {
              cacheAuthenticatedIdentity(peerNodeId, identity);
          })),
      clientTransports_(std::move(clientTransports)) {
    incomingMessages_->setWorkerCount(1);
    incomingMessages_->start();
        rebuildAuthorizationQueue();
    signatureCheckQueue_->setWorkerCount(1);
    signatureCheckQueue_->start();
    handshakeQueue_->setWorkerCount(4);
    handshakeQueue_->start();
    authnQueue_->setWorkerCount(4);
    authnQueue_->start();
    registerTransportCallbacks();
}

ResourceManager::~ResourceManager() {
    if (incomingMessages_ != nullptr) {
        incomingMessages_->stop();
    }

    // Stop signatureCheckQueue_ before authzQueue_ — it is the feeder for authzQueue_
    // via handleVerifiedMessage. Once stopped, no new messages can race into authzQueue_.
    if (signatureCheckQueue_ != nullptr) {
        signatureCheckQueue_->stop();
    }

    rsp::MessageQueueHandle authzQueue;
    {
        std::lock_guard<std::mutex> lock(authzQueueMutex_);
        authzQueue = std::move(authzQueue_);
    }
    if (authzQueue != nullptr) {
        authzQueue->stop();
    }

    if (handshakeQueue_ != nullptr) {
        handshakeQueue_->stop();
    }

    if (authnQueue_ != nullptr) {
        authnQueue_->stop();
    }

    stopNodeQueues();

    std::vector<ActiveEncodingState> encodings;
    {
        std::lock_guard<std::mutex> lock(encodingsMutex_);
        encodings.swap(activeEncodings_);
    }

    for (const auto& activeEncoding : encodings) {
        if (activeEncoding.signingQueue != nullptr) {
            activeEncoding.signingQueue->stop();
        }
        if (activeEncoding.encoding != nullptr) {
            activeEncoding.encoding->stop();
        }
    }

    for (const auto& transport : clientTransports_) {
        if (transport != nullptr) {
            transport->stop();
        }
    }
}

int ResourceManager::run() const {
    return 0;
}

void ResourceManager::rebuildAuthorizationQueue() {
    rsp::MessageQueueHandle oldQueue;
    rsp::MessageQueueHandle newQueue = std::make_shared<rsp::message_queue::MessageQueueAuthZ>(
        [this](rsp::proto::RSPMessage message) { handleAuthorizedMessage(std::move(message)); },
        [this](rsp::proto::RSPMessage message) { handleAuthorizationFailure(std::move(message)); },
        [this](const rsp::NodeID& nodeId) { return getAuthorizationEndorsements(nodeId); },
        authorizationTree());
    newQueue->setWorkerCount(1);
    newQueue->start();

    {
        std::lock_guard<std::mutex> lock(authzQueueMutex_);
        oldQueue = std::move(authzQueue_);
        authzQueue_ = std::move(newQueue);
    }

    if (oldQueue != nullptr) {
        oldQueue->stop();
    }
}

bool ResourceManager::handleNodeSpecificMessage(const rsp::proto::RSPMessage& message) {
    if (message.has_resource_advertisement()) {
        const auto nodeId = rsp::senderNodeIdFromMessage(message);
        if (!nodeId.has_value()) {
            return false;
        }

        if (message.resource_advertisement().records_size() == 0) {
            eraseResourceAdvertisement(*nodeId);
            return true;
        }

        schemaRegistry_.ingest(message.resource_advertisement());

        std::lock_guard<std::mutex> lock(resourceAdvertisementsMutex_);
        resourceAdvertisements_[*nodeId] = message.resource_advertisement();
        return true;
    }

    if (message.has_schema_request()) {
        const auto requesterNodeId = rsp::senderNodeIdFromMessage(message);
        if (!requesterNodeId.has_value()) {
            return false;
        }

        const auto& request = message.schema_request();

        rsp::proto::RSPMessage reply;
        *reply.mutable_destination() = toProtoNodeId(*requesterNodeId);

        auto* schemaReply = reply.mutable_schema_reply();
        if (!request.proto_file_name().empty()) {
            auto schema = schemaRegistry_.findByProtoFile(request.proto_file_name());
            if (schema.has_value()) {
                *schemaReply->add_schemas() = std::move(*schema);
            }
        } else if (!request.schema_hash().empty()) {
            auto schema = schemaRegistry_.findByHash(request.schema_hash());
            if (schema.has_value()) {
                *schemaReply->add_schemas() = std::move(*schema);
            }
        } else {
            // No filter: return all known schemas.
            for (auto& s : schemaRegistry_.allSchemas()) {
                *schemaReply->add_schemas() = std::move(s);
            }
        }

        const auto queue = outputQueue();
        return queue != nullptr && queue->push(std::move(reply));
    }

    if (message.has_resource_query()) {
        const auto requesterNodeId = rsp::senderNodeIdFromMessage(message);
        if (!requesterNodeId.has_value()) {
            return false;
        }

        rsp::proto::RSPMessage reply;
        *reply.mutable_destination() = toProtoNodeId(*requesterNodeId);

        {
            std::lock_guard<std::mutex> lock(resourceAdvertisementsMutex_);
            for (const auto& [nodeId, advertisement] : resourceAdvertisements_) {
                for (const auto& record : advertisement.records()) {
                    auto* replyRecord = reply.mutable_resource_advertisement()->add_records();
                    *replyRecord = record;
                    setNodeIdIfPresent(replyRecord, nodeId);
                }
            }
        }

        const auto queue = outputQueue();
        return queue != nullptr && queue->push(std::move(reply));
    }

    return false;
}

void ResourceManager::handleOutputMessage(rsp::proto::RSPMessage message) {
    if (!sendLocalMessage(message)) {
        std::cerr << "ResourceManager failed to route outgoing message" << std::endl;
    }
}

bool ResourceManager::sendLocalMessage(const rsp::proto::RSPMessage& message) const {
    if (!message.has_destination() || message.destination().value().empty()) {
        return false;
    }

    ActiveEncodingState selectedEncoding;

    {
        std::lock_guard<std::mutex> lock(encodingsMutex_);
        for (const auto& activeEncoding : activeEncodings_) {
            if (activeEncoding.encoding == nullptr) {
                continue;
            }

            const auto peerNodeId = activeEncoding.encoding->peerNodeID();
            if (!peerNodeId.has_value()) {
                continue;
            }

            if (toProtoNodeId(*peerNodeId).value() == message.destination().value()) {
                selectedEncoding = activeEncoding;
                break;
            }
        }
    }

    if (selectedEncoding.encoding == nullptr || selectedEncoding.signingQueue == nullptr) {
        return false;
    }

    return selectedEncoding.signingQueue->push(message);
}

void ResourceManager::eraseResourceAdvertisement(const rsp::NodeID& nodeId) const {
    std::lock_guard<std::mutex> lock(resourceAdvertisementsMutex_);
    resourceAdvertisements_.erase(nodeId);
}

void ResourceManager::addClientTransport(const rsp::transport::ListeningTransportHandle& transport) {
    registerTransportCallback(transport);
    clientTransports_.push_back(transport);
}

size_t ResourceManager::clientTransportCount() const {
    return clientTransports_.size();
}

void ResourceManager::setNewEncodingCallback(NewEncodingCallback callback) {
    std::lock_guard<std::mutex> lock(newEncodingCallbackMutex_);
    newEncodingCallback_ = std::move(callback);
}

size_t ResourceManager::activeEncodingCount() const {
    std::lock_guard<std::mutex> lock(encodingsMutex_);
    return activeEncodings_.size();
}

size_t ResourceManager::resourceAdvertisementCount() const {
    std::lock_guard<std::mutex> lock(resourceAdvertisementsMutex_);
    return resourceAdvertisements_.size();
}

bool ResourceManager::hasResourceAdvertisement(const rsp::NodeID& nodeId) const {
    std::lock_guard<std::mutex> lock(resourceAdvertisementsMutex_);
    return resourceAdvertisements_.find(nodeId) != resourceAdvertisements_.end();
}

std::optional<rsp::proto::ResourceAdvertisement> ResourceManager::resourceAdvertisement(const rsp::NodeID& nodeId) const {
    std::lock_guard<std::mutex> lock(resourceAdvertisementsMutex_);
    const auto iterator = resourceAdvertisements_.find(nodeId);
    if (iterator == resourceAdvertisements_.end()) {
        return std::nullopt;
    }

    return iterator->second;
}

size_t ResourceManager::schemaCount() const {
    return schemaRegistry_.size();
}

bool ResourceManager::sendToConnection(size_t index, const rsp::proto::RSPMessage& message) const {
    ActiveEncodingState selectedEncoding;

    {
        std::lock_guard<std::mutex> lock(encodingsMutex_);
        if (index >= activeEncodings_.size()) {
            return false;
        }

        selectedEncoding = activeEncodings_[index];
    }

    if (selectedEncoding.encoding == nullptr || selectedEncoding.signingQueue == nullptr) {
        return false;
    }

    const bool queued = selectedEncoding.signingQueue->push(message);
    if (queued && rsp::ping_trace::isEnabled() && message.has_ping_request()) {
        rsp::ping_trace::recordForMessage(message, "rm_forward_send_enqueued");
    }

    return queued;
}

bool ResourceManager::routeAndSend(const rsp::proto::RSPMessage& message) const {
    if (!message.has_destination() || message.destination().value().empty()) {
        return false;
    }

    const auto destinationNodeId = fromProtoNodeId(message.destination());

    rsp::encoding::EncodingHandle selectedEncoding;

    {
        std::lock_guard<std::mutex> lock(encodingsMutex_);
        for (const auto& activeEncoding : activeEncodings_) {
            if (activeEncoding.encoding == nullptr) {
                continue;
            }

            const auto peerNodeId = activeEncoding.encoding->peerNodeID();
            if (!peerNodeId.has_value()) {
                continue;
            }

            if (toProtoNodeId(*peerNodeId).value() == message.destination().value()) {
                selectedEncoding = activeEncoding.encoding;
                break;
            }
        }
    }

    if (selectedEncoding == nullptr) {
        if (destinationNodeId.has_value()) {
            eraseResourceAdvertisement(*destinationNodeId);
        }
        return false;
    }

    const auto outgoingMessages = selectedEncoding->outgoingMessages();
    const bool sent = outgoingMessages != nullptr && outgoingMessages->push(message);
    if (!sent && destinationNodeId.has_value()) {
        eraseResourceAdvertisement(*destinationNodeId);
    }

    return sent;
}

bool ResourceManager::isForThisNode(const rsp::proto::RSPMessage& message) const {
    if (!message.has_destination()) {
        return true;
    }

    return message.destination().value() == toProtoNodeId(keyPair().nodeID()).value();
}

bool ResourceManager::tryDequeueMessage(rsp::proto::RSPMessage& message) const {
    return incomingMessages_ != nullptr && incomingMessages_->tryPop(message);
}

size_t ResourceManager::pendingMessageCount() const {
    return incomingMessages_ == nullptr ? 0 : incomingMessages_->size();
}

void ResourceManager::registerTransportCallbacks() {
    for (const auto& transport : clientTransports_) {
        registerTransportCallback(transport);
    }
}

void ResourceManager::registerTransportCallback(const rsp::transport::ListeningTransportHandle& transport) {
    if (transport == nullptr) {
        return;
    }

    transport->setNewConnectionCallback([this](const rsp::transport::ConnectionHandle& connection) {
        enqueueAcceptedConnection(connection);
    });
}

void ResourceManager::enqueueAcceptedConnection(const rsp::transport::ConnectionHandle& connection) {
    if (connection == nullptr) {
        return;
    }

    if (handshakeQueue_ == nullptr || !handshakeQueue_->push(connection)) {
        connection->close();
    }
}

void ResourceManager::handleHandshakeSuccess(const rsp::encoding::EncodingHandle& encoding) {
    if (encoding == nullptr) {
        return;
    }

    if (authnQueue_ == nullptr || !authnQueue_->push(encoding)) {
        encoding->stop();
    }
}

void ResourceManager::handleAuthNSuccess(const rsp::encoding::EncodingHandle& encoding) {
    if (encoding == nullptr) {
        return;
    }

    if (!encoding->start()) {
        encoding->stop();
        return;
    }

    const auto signingQueue = std::make_shared<rsp::MessageQueueSign>(
        [encoding](rsp::proto::RSPMessage message) {
            const auto outgoingMessages = encoding == nullptr ? nullptr : encoding->outgoingMessages();
            if (outgoingMessages == nullptr || !outgoingMessages->push(std::move(message))) {
                std::cerr << "ResourceManager failed to enqueue signed outbound message" << std::endl;
            }
        },
        [](rsp::proto::RSPMessage, std::string reason) {
            std::cerr << "ResourceManager failed to sign outbound message: " << reason << std::endl;
        },
        keyPair().duplicate());
    signingQueue->setWorkerCount(1);
    signingQueue->start();

    {
        std::lock_guard<std::mutex> lock(encodingsMutex_);
        activeEncodings_.push_back(ActiveEncodingState{encoding, signingQueue});
    }

    NewEncodingCallback callback;
    {
        std::lock_guard<std::mutex> lock(newEncodingCallbackMutex_);
        callback = newEncodingCallback_;
    }

    if (callback) {
        callback(encoding);
    }
}

void ResourceManager::handleAuthNFailure(const rsp::encoding::EncodingHandle& encoding) {
    if (encoding != nullptr) {
        encoding->stop();
    }
}

void ResourceManager::handleVerifiedMessage(rsp::proto::RSPMessage message) {
    rsp::MessageQueueHandle queue;
    {
        std::lock_guard<std::mutex> lock(authzQueueMutex_);
        queue = authzQueue_;
    }
    if (queue == nullptr || !queue->push(std::move(message))) {
        std::cerr << "ResourceManager failed to enqueue verified message for authorization" << std::endl;
    }
}

void ResourceManager::handleSignatureFailure(rsp::proto::RSPMessage message, const std::string& reason) {
    sendSignatureFailure(message, reason);
}

void ResourceManager::handleAuthorizedMessage(rsp::proto::RSPMessage message) {
    if (isForThisNode(message)) {
        if (!enqueueInput(std::move(message))) {
            std::cerr << "ResourceManager failed to enqueue local message on input queue" << std::endl;
        }
        return;
    }

    observeMessage(message);

    if (!routeAndSend(message)) {
        std::cerr << "ResourceManager failed to route incoming message" << std::endl;
    }
}

void ResourceManager::handleAuthorizationFailure(rsp::proto::RSPMessage message) {
    sendEndorsementNeeded(message);
}

void ResourceManager::cacheAuthenticatedIdentity(const rsp::NodeID& peerNodeId, const rsp::proto::Identity& identity) {
    rsp::proto::RSPMessage message;
    *message.mutable_source() = toProtoNodeId(peerNodeId);
    *message.add_identities() = identity;
    observeMessage(message);
}

std::shared_ptr<const rsp::KeyPair> ResourceManager::verificationKeyForNodeId(const rsp::NodeID& nodeId) const {
    if (nodeId == keyPair().nodeID()) {
        return std::make_shared<rsp::KeyPair>(keyPair().duplicate());
    }

    const auto identity = identityCache().get(nodeId);
    if (!identity.has_value()) {
        return nullptr;
    }

    try {
        return std::make_shared<rsp::KeyPair>(rsp::KeyPair::fromPublicKey(identity->public_key()));
    } catch (const std::exception&) {
        return nullptr;
    }
}

std::vector<rsp::Endorsement> ResourceManager::getAuthorizationEndorsements(const rsp::NodeID&) const {
    return {};
}

rsp::proto::ERDAbstractSyntaxTree ResourceManager::authorizationTree() const {
    rsp::proto::ERDAbstractSyntaxTree tree;
    tree.mutable_true_value();
    return tree;
}

void ResourceManager::sendSignatureFailure(const rsp::proto::RSPMessage& rejectedMessage, const std::string& reason) const {
    const auto requesterNodeId = rsp::senderNodeIdFromMessage(rejectedMessage);
    if (!requesterNodeId.has_value()) {
        return;
    }

    rsp::proto::RSPMessage reply;
    *reply.mutable_destination() = toProtoNodeId(*requesterNodeId);
    if (rejectedMessage.has_nonce()) {
        *reply.mutable_nonce() = rejectedMessage.nonce();
    } else {
        reply.mutable_nonce()->set_value(randomMessageNonce());
    }
    reply.mutable_error()->set_error_code(rsp::proto::SIGNATURE_FAILED);
    reply.mutable_error()->set_message(reason);

    if (!sendLocalMessage(reply)) {
        std::cerr << "ResourceManager failed to send SIGNATURE_FAILED reply" << std::endl;
    }
}

void ResourceManager::sendEndorsementNeeded(const rsp::proto::RSPMessage& rejectedMessage) const {
    const auto requesterNodeId = rsp::senderNodeIdFromMessage(rejectedMessage);
    if (!requesterNodeId.has_value()) {
        return;
    }

    rsp::proto::RSPMessage reply;
    *reply.mutable_destination() = toProtoNodeId(*requesterNodeId);
    if (rejectedMessage.has_nonce()) {
        *reply.mutable_nonce() = rejectedMessage.nonce();
    } else {
        reply.mutable_nonce()->set_value(randomMessageNonce());
    }

    auto* endorsementNeeded = reply.mutable_endorsement_needed();
    *endorsementNeeded->mutable_message_nonce() = reply.nonce();
    *endorsementNeeded->mutable_tree() = authorizationTree();

    if (!sendLocalMessage(reply)) {
        std::cerr << "ResourceManager failed to send EndorsementNeeded reply" << std::endl;
    }
}

}  // namespace rsp::resource_manager