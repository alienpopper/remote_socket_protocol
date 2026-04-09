#pragma once

#include "common/base_types.hpp"
#include "common/keypair.hpp"
#include "common/message_queue/mq.hpp"

#include <functional>
#include <memory>
#include <string>

namespace rsp {

// Returns the keypair for a given NodeID, or nullptr if not found.
using GetKeyFunction = std::function<std::shared_ptr<const KeyPair>(const NodeID&)>;

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
    MessageQueueCheckSignature(std::function<void(rsp::proto::RSPMessage)> onSuccess,
                               std::function<void(rsp::proto::RSPMessage, std::string)> onFailure,
                               GetKeyFunction getKeyForNodeID);

    // Convenience overload: wraps keyPair in a lambda that returns it only when
    // the SignatureBlock signer NodeID matches the keypair's own NodeID.
    MessageQueueCheckSignature(std::function<void(rsp::proto::RSPMessage)> onSuccess,
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

}  // namespace rsp
