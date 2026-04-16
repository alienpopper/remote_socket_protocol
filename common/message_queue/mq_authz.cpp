#include "common/message_queue/mq_authz.hpp"

#include "common/message_queue/mq_signing.hpp"
#include "resource_manager/schema_registry.hpp"

#include <iostream>

namespace rsp::message_queue {

MessageQueueAuthZ::MessageQueueAuthZ(SuccessCallback success,
                                     FailureCallback failure,
                                     GetEndorsementsCallback getEndorsements,
                                     rsp::proto::ERDAbstractSyntaxTree authorizationTree,
                                     const rsp::resource_manager::SchemaRegistry* schemaRegistry)
    : success_(std::move(success)),
      failure_(std::move(failure)),
      getEndorsements_(std::move(getEndorsements)),
      authorizationTree_(std::move(authorizationTree)),
      schemaRegistry_(schemaRegistry) {
}

void MessageQueueAuthZ::handleMessage(Message message, rsp::MessageQueueSharedState&) {
    const bool trace = rsp::messageTraceEnabled(message);
    if (trace) {
        std::cerr << "[mq_authz] entry" << std::endl;
    }
    const auto sourceNodeId = rsp::senderNodeIdFromMessage(message);
    if (!sourceNodeId.has_value()) {
        if (trace) {
            std::cerr << "[mq_authz] reject: sender node id missing" << std::endl;
        }
        failure_(std::move(message));
        return;
    }

    std::vector<rsp::Endorsement> endorsements;
    try {
        endorsements = getEndorsements_(*sourceNodeId);
    } catch (const std::exception&) {
        if (trace) {
            std::cerr << "[mq_authz] reject: endorsement lookup failed for sender "
                      << sourceNodeId->toString() << std::endl;
        }
        failure_(std::move(message));
        return;
    }

    try {
        // Take an immutable snapshot of schemas before evaluating so
        // that descriptor changes cannot affect a mid-flight evaluation.
        std::optional<rsp::resource_manager::SchemaSnapshot> snap;
        const rsp::resource_manager::SchemaSnapshot* snapPtr = nullptr;
        if (schemaRegistry_ != nullptr) {
            snap = schemaRegistry_->snapshot();
            snapPtr = &*snap;
        }
        const auto reducedRequirement = rsp::reduceRequirementTree(authorizationTree_, endorsements, &message, snapPtr);
        if (reducedRequirement.node_type_case() == rsp::proto::ERDAbstractSyntaxTree::NODE_TYPE_NOT_SET) {
            if (trace) {
                std::cerr << "[mq_authz] success" << std::endl;
            }
            success_(std::move(message));
            return;
        }
        if (trace) {
            std::cerr << "[mq_authz] reject: endorsements do not satisfy requirement for sender "
                      << sourceNodeId->toString() << std::endl;
        }
    } catch (const std::exception&) {
        if (trace) {
            std::cerr << "[mq_authz] reject: requirement reduction failed for sender "
                      << sourceNodeId->toString() << std::endl;
        }
        failure_(std::move(message));
        return;
    }

    failure_(std::move(message));
}

void MessageQueueAuthZ::handleQueueFull(size_t, size_t, const Message& message) {
    if (rsp::messageTraceEnabled(message)) {
        std::cerr << "[mq_authz] queue_full" << std::endl;
    }
    failure_(message);
}

}  // namespace rsp::message_queue