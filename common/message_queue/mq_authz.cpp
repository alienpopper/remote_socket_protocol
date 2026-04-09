#include "common/message_queue/mq_authz.hpp"

#include <cstring>

namespace {

std::optional<rsp::NodeID> nodeIdFromSourceField(const rsp::proto::NodeId& protoId) {
    if (protoId.value().size() != 16) {
        return std::nullopt;
    }

    uint64_t high = 0;
    uint64_t low = 0;
    std::memcpy(&high, protoId.value().data(), sizeof(high));
    std::memcpy(&low, protoId.value().data() + sizeof(high), sizeof(low));
    return rsp::NodeID(high, low);
}

rsp::proto::EndorsementNeeded makeRequirement(const rsp::proto::ERDAbstractSyntaxTree& tree) {
    rsp::proto::EndorsementNeeded requirement;
    *requirement.mutable_tree() = tree;
    return requirement;
}

bool endorsementAuthorizes(const rsp::Endorsement& endorsement,
                          const rsp::NodeID& subjectNodeId,
                          const rsp::proto::ERDAbstractSyntaxTree& authorizationTree) {
    if (endorsement.subject() != subjectNodeId) {
        return false;
    }

    if (endorsement.validUntil() < rsp::DateTime()) {
        return false;
    }

    return rsp::endorsementMatchesRequirement(makeRequirement(authorizationTree), endorsement);
}

}  // namespace

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
    if (!message.has_source()) {
        failure_(std::move(message));
        return;
    }

    const auto sourceNodeId = nodeIdFromSourceField(message.source());
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
        const auto reducedRequirement = rsp::reduceRequirementTree(authorizationTree_, endorsements);
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