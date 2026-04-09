#pragma once

#include "common/endorsement/endorsement.hpp"
#include "common/message_queue/mq.hpp"

#include <functional>
#include <vector>

namespace rsp::message_queue {

class MessageQueueAuthZ : public rsp::RSPMessageQueue {
public:
    using SuccessCallback = std::function<void(rsp::proto::RSPMessage)>;
    using FailureCallback = std::function<void(rsp::proto::RSPMessage)>;
    using GetEndorsementsCallback = std::function<std::vector<rsp::Endorsement>(const rsp::NodeID& nodeId)>;

    MessageQueueAuthZ(SuccessCallback success,
                      FailureCallback failure,
                      GetEndorsementsCallback getEndorsements,
                      rsp::proto::ERDAbstractSyntaxTree authorizationTree);

protected:
    void handleMessage(Message message, rsp::MessageQueueSharedState& sharedState) override;
    void handleQueueFull(size_t currentSize, size_t limit, const Message& message) override;

private:
    SuccessCallback success_;
    FailureCallback failure_;
    GetEndorsementsCallback getEndorsements_;
    rsp::proto::ERDAbstractSyntaxTree authorizationTree_;
};

}  // namespace rsp::message_queue