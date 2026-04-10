#include "common/message_queue/mq_authz.hpp"

#include "common/message_queue/mq_signing.hpp"

namespace rsp::message_queue {

MessageQueueAuthZ::MessageQueueAuthZ(SuccessCallback success,
                                     FailureCallback failure,
                                     GetEndorsementsCallback getEndorsements,
                                     rsp::proto::ERDAbstractSyntaxTree authorizationTree)
    : success_(std::move(success)),
      failure_(std::move(failure)),
      getEndorsements_(std::move(getEndorsements)),
      authorizationTree_(std::move(authorizationTree)) {
}

void MessageQueueAuthZ::handleMessage(Message message, rsp::MessageQueueSharedState&) {
    const auto sourceNodeId = rsp::senderNodeIdFromMessage(message);
    if (!sourceNodeId.has_value()) {
        failure_(std::move(message));
        return;
    }

    std::vector<rsp::Endorsement> endorsements;
    try {
        endorsements = getEndorsements_(*sourceNodeId);
    } catch (const std::exception&) {
        failure_(std::move(message));
        return;
    }

    try {
        const auto reducedRequirement = rsp::reduceRequirementTree(authorizationTree_, endorsements, &message);
        if (reducedRequirement.node_type_case() == rsp::proto::ERDAbstractSyntaxTree::NODE_TYPE_NOT_SET) {
            success_(std::move(message));
            return;
        }
    } catch (const std::exception&) {
        failure_(std::move(message));
        return;
    }

    failure_(std::move(message));
}

void MessageQueueAuthZ::handleQueueFull(size_t, size_t, const Message& message) {
    failure_(message);
}

}  // namespace rsp::message_queue