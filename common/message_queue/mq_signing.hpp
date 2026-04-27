#pragma once

#include "common/base_types.hpp"
#include "common/keypair.hpp"
#include "common/message_queue/mq.hpp"

#include <array>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace rsp {

// Returns the keypair for a given NodeID, or nullptr if not found.
using GetKeyFunction = std::function<std::shared_ptr<const KeyPair>(const NodeID&)>;

using MessageHash = std::array<uint8_t, 32>;

// Computes the canonical packet hash for an RSPMessage. The outer signature
// field is excluded so the result can be used as packet signature input.
MessageHash computeMessageHash(const rsp::proto::RSPMessage& message);
Buffer messageSignatureInput(const rsp::proto::RSPMessage& message);
rsp::proto::SignatureBlock signMessage(const KeyPair& keyPair, const rsp::proto::RSPMessage& message);
bool verifyMessageSignature(const KeyPair& keyPair,
                            const rsp::proto::RSPMessage& message,
                            const rsp::proto::SignatureBlock& signatureBlock);
std::optional<NodeID> nodeIdFromSourceField(const rsp::proto::NodeId& protoId);
std::optional<NodeID> nodeIdFromSignerField(const rsp::proto::NodeId& protoId);
std::optional<NodeID> senderNodeIdFromMessage(const rsp::proto::RSPMessage& message);
bool messageTraceEnabled(const rsp::proto::RSPMessage& message);
void copyMessageTrace(const rsp::proto::RSPMessage& source, rsp::proto::RSPMessage& destination);

// Signs each incoming RSPMessage using the keypair returned by getKeyForNodeID,
// keyed on the message's source NodeID. On success, calls onSuccess with the
// signed message. On failure, calls onFailure with the original message and a
// description of the failure.
class MessageQueueSign : public RSPMessageQueue {
public:
    MessageQueueSign(std::function<void(rsp::proto::RSPMessage)> onSuccess,
                     std::function<void(rsp::proto::RSPMessage, std::string)> onFailure,
                     GetKeyFunction getKeyForNodeID);

    // Convenience overload: wraps keyPair in a lambda that returns it only when
    // the message source NodeID matches the keypair's own NodeID.
    MessageQueueSign(std::function<void(rsp::proto::RSPMessage)> onSuccess,
                     std::function<void(rsp::proto::RSPMessage, std::string)> onFailure,
                     KeyPair keyPair);

protected:
    void handleMessage(Message message, MessageQueueSharedState& sharedState) override;
    void handleQueueFull(size_t currentSize, size_t limit, const Message& rejected) override;

private:
    std::function<void(rsp::proto::RSPMessage)> onSuccess_;
    std::function<void(rsp::proto::RSPMessage, std::string)> onFailure_;
    GetKeyFunction getKey_;
};

// Verifies the signature on each incoming RSPMessage using the keypair returned
// by getKeyForNodeID, keyed on the SignatureBlock's signer NodeID. On success,
// calls onSuccess with the verified message. On failure, calls onFailure with
// the original message and a description of the failure.
class MessageQueueCheckSignature : public RSPMessageQueue {
public:
    enum class FailureKind {
        MissingSignature,
        InvalidSignerNodeId,
        MissingVerificationKey,
        SignatureVerificationFailed,
        InternalError,
    };

    struct Failure {
        FailureKind kind = FailureKind::InternalError;
        std::string reason;
        std::optional<NodeID> signerNodeId;
    };

    using SuccessCallback = std::function<void(rsp::proto::RSPMessage)>;
    using FailureCallback = std::function<void(rsp::proto::RSPMessage, Failure)>;
    using LegacyFailureCallback = std::function<void(rsp::proto::RSPMessage, std::string)>;

    MessageQueueCheckSignature(SuccessCallback onSuccess,
                               FailureCallback onFailure,
                               GetKeyFunction getKeyForNodeID);

    MessageQueueCheckSignature(SuccessCallback onSuccess,
                               LegacyFailureCallback onFailure,
                               GetKeyFunction getKeyForNodeID);

    // Convenience overload: wraps keyPair in a lambda that returns it only when
    // the SignatureBlock signer NodeID matches the keypair's own NodeID.
    MessageQueueCheckSignature(SuccessCallback onSuccess,
                               FailureCallback onFailure,
                               KeyPair keyPair);

    MessageQueueCheckSignature(SuccessCallback onSuccess,
                               LegacyFailureCallback onFailure,
                               KeyPair keyPair);

protected:
    void handleMessage(Message message, MessageQueueSharedState& sharedState) override;
    void handleQueueFull(size_t currentSize, size_t limit, const Message& rejected) override;

private:
    SuccessCallback onSuccess_;
    FailureCallback onFailure_;
    GetKeyFunction getKey_;
};

struct IdentityResolutionRequest {
    rsp::proto::RSPMessage message;
    rsp::NodeID signerNodeId;
};

class MessageQueueRequestIdentity : public MessageQueue<IdentityResolutionRequest> {
public:
    using Message = typename MessageQueue<IdentityResolutionRequest>::Message;
    using ReplayCallback = std::function<bool(rsp::proto::RSPMessage)>;
    using FailureCallback = std::function<void(rsp::proto::RSPMessage, std::string)>;
    using RequestIdentityCallback = std::function<bool(const rsp::NodeID&)>;
    using HasIdentityCallback = std::function<bool(const rsp::NodeID&)>;

    struct Limits {
        size_t maxPendingMessages = 512;
        size_t maxPendingPerSigner = 32;
        std::chrono::milliseconds resolveTimeout{1500};
        std::chrono::milliseconds pollInterval{25};
    };

    MessageQueueRequestIdentity(ReplayCallback replay,
                                FailureCallback onFailure,
                                RequestIdentityCallback requestIdentity,
                                HasIdentityCallback hasIdentity,
                                Limits limits);
    MessageQueueRequestIdentity(ReplayCallback replay,
                                FailureCallback onFailure,
                                RequestIdentityCallback requestIdentity,
                                HasIdentityCallback hasIdentity);
    ~MessageQueueRequestIdentity() override;

    void notifyIdentityObserved(const rsp::NodeID& nodeId);

protected:
    void handleMessage(Message message, MessageQueueSharedState& sharedState) override;
    void handleQueueFull(size_t currentSize, size_t limit, const Message& rejected) override;

private:
    struct PendingSignerState {
        std::deque<rsp::proto::RSPMessage> messages;
        bool resolverActive = false;
        std::condition_variable condition;
    };

    void resolveSigner(const rsp::NodeID& nodeId);
    std::vector<rsp::proto::RSPMessage> drainSignerMessages(const rsp::NodeID& nodeId);
    std::vector<rsp::proto::RSPMessage> drainSignerMessagesLocked(const rsp::NodeID& nodeId);
    void replayMessages(std::vector<rsp::proto::RSPMessage> messages) const;
    void failMessages(std::vector<rsp::proto::RSPMessage> messages, const std::string& reason) const;

    mutable std::mutex pendingMutex_;
    std::map<rsp::NodeID, PendingSignerState> pendingBySigner_;
    size_t totalPendingMessages_ = 0;
    bool stopping_ = false;

    mutable std::mutex resolverThreadsMutex_;
    std::vector<std::thread> resolverThreads_;

    ReplayCallback replay_;
    FailureCallback onFailure_;
    RequestIdentityCallback requestIdentity_;
    HasIdentityCallback hasIdentity_;
    Limits limits_;
};

}  // namespace rsp
