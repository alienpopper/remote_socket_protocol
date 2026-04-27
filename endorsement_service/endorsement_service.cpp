#include "endorsement_service/endorsement_service.hpp"

#include "common/endorsement/endorsement.hpp"
#include "common/endorsement/well_known_endorsements.h"
#include "common/message_queue/mq_signing.hpp"
#include "common/service_message.hpp"

#include <cstring>

namespace {

[[maybe_unused]] const rsp::GUID kWellKnownEndorsements[] = {
    ETYPE_ACCESS,
    EVALUE_ACCESS_NETWORK,
    EVALUE_REGISTER_NAMES,
    ETYPE_ROLE,
    EVALUE_ROLE_CLIENT,
    EVALUE_ROLE_RESOURCE_MANAGER,
    EVALUE_ROLE_RESOURCE_SERVICE,
    EVALUE_ROLE_ENDORSEMENT_SERVICE,
    EVALUE_ROLE_NAME_SERVICE,
};

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

bool buffersEqual(const rsp::Buffer& lhs, const rsp::Buffer& rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    if (lhs.empty()) {
        return true;
    }
    return std::memcmp(lhs.data(), rhs.data(), lhs.size()) == 0;
}

}  // namespace

namespace rsp::endorsement_service {

EndorsementService::Ptr EndorsementService::create() {
    return Ptr(new EndorsementService(KeyPair::generateP256()));
}

EndorsementService::Ptr EndorsementService::create(KeyPair keyPair) {
    return Ptr(new EndorsementService(std::move(keyPair)));
}

EndorsementService::EndorsementService(KeyPair keyPair)
    : rsp::client::full::RSPClient(std::move(keyPair)) {
}

void EndorsementService::setConfiguredEndorsements(std::vector<ConfiguredEndorsement> configuredEndorsements) {
    std::lock_guard<std::mutex> lock(configuredEndorsementsMutex_);
    configuredEndorsements_ = std::move(configuredEndorsements);
}

size_t EndorsementService::configuredEndorsementCount() const {
    std::lock_guard<std::mutex> lock(configuredEndorsementsMutex_);
    return configuredEndorsements_.size();
}

bool EndorsementService::handleNodeSpecificMessage(const rsp::proto::RSPMessage& message) {
    if (message.has_identity() || message.identities_size() > 0) {
        return true;
    }

    if (rsp::hasServiceMessage<rsp::proto::BeginEndorsementRequest>(message)) {
        return handleBeginEndorsementRequest(message);
    }

    return false;
}

EndorsementService::ConfiguredEndorsementMatch EndorsementService::findMatchingConfiguredEndorsement(
    const rsp::NodeID& requestorNodeId,
    const rsp::Endorsement& requestedValues) const {
    ConfiguredEndorsementMatch match;

    std::lock_guard<std::mutex> lock(configuredEndorsementsMutex_);
    match.hasConfiguredEndorsements = !configuredEndorsements_.empty();
    for (const auto& configuredEndorsement : configuredEndorsements_) {
        if (configuredEndorsement.requestor.has_value() &&
            configuredEndorsement.requestor.value() != requestorNodeId) {
            continue;
        }

        if (configuredEndorsement.endorsementType != requestedValues.endorsementType()) {
            continue;
        }

        if (!buffersEqual(configuredEndorsement.endorsementValue, requestedValues.endorsementValue())) {
            continue;
        }

        match.endorsement = configuredEndorsement;
        break;
    }

    return match;
}

bool EndorsementService::handleBeginEndorsementRequest(const rsp::proto::RSPMessage& message) {
    rsp::proto::RSPMessage reply;
    const auto sourceNodeId = rsp::senderNodeIdFromMessage(message);
    if (sourceNodeId.has_value()) {
        *reply.mutable_destination() = toProtoNodeId(*sourceNodeId);
    }

    rsp::proto::EndorsementDone done;
    done.set_status(rsp::proto::ENDORSEMENT_FAILED);

    try {
        if (!sourceNodeId.has_value()) {
            rsp::packServiceMessage(reply, done);
            return send(reply);
        }

        const auto cachedIdentity = identityCache().get(*sourceNodeId);
        if (!cachedIdentity.has_value()) {
            done.set_status(rsp::proto::ENDORSEMENT_UNKNOWN_IDENTITY);
            rsp::packServiceMessage(reply, done);
            return send(reply);
        }

        rsp::proto::BeginEndorsementRequest request;
        if (!rsp::unpackServiceMessage(message, &request)) {
            rsp::packServiceMessage(reply, done);
            return send(reply);
        }

        if (!request.has_requested_values()) {
            done.set_status(rsp::proto::ENDORSEMENT_INVALID_SIGNATURE);
            rsp::packServiceMessage(reply, done);
            return send(reply);
        }

        const rsp::KeyPair requesterPublicKey = rsp::KeyPair::fromPublicKey(cachedIdentity->public_key());
        if (requesterPublicKey.nodeID() != *sourceNodeId) {
            done.set_status(rsp::proto::ENDORSEMENT_INVALID_SIGNATURE);
            rsp::packServiceMessage(reply, done);
            return send(reply);
        }

        const rsp::Endorsement requestedValues = rsp::Endorsement::fromProto(request.requested_values());
        if (requestedValues.subject() != *sourceNodeId ||
            requestedValues.endorsementService() != *sourceNodeId ||
            !requestedValues.verifySignature(requesterPublicKey)) {
            done.set_status(rsp::proto::ENDORSEMENT_INVALID_SIGNATURE);
            rsp::packServiceMessage(reply, done);
            return send(reply);
        }

        const auto configuredMatch = findMatchingConfiguredEndorsement(*sourceNodeId, requestedValues);
        if (configuredMatch.hasConfiguredEndorsements && !configuredMatch.endorsement.has_value()) {
            done.set_status(rsp::proto::ENDORSEMENT_FAILED);
            rsp::packServiceMessage(reply, done);
            return send(reply);
        }

        rsp::DateTime validUntil;
        rsp::GUID endorsementType = requestedValues.endorsementType();
        rsp::Buffer endorsementValue = requestedValues.endorsementValue();
        double validForSeconds = DAYS(1);

        if (configuredMatch.endorsement.has_value()) {
            endorsementType = configuredMatch.endorsement->endorsementType;
            endorsementValue = configuredMatch.endorsement->endorsementValue;
            if (configuredMatch.endorsement->validForSeconds > 0.0) {
                validForSeconds = configuredMatch.endorsement->validForSeconds;
            }
        }

        validUntil += validForSeconds;
        const rsp::Endorsement issuedEndorsement = rsp::Endorsement::createSigned(
            keyPair(),
            requestedValues.subject(),
            endorsementType,
            endorsementValue,
            validUntil);

        done.set_status(rsp::proto::ENDORSEMENT_SUCCESS);
        *done.mutable_new_endorsement() = issuedEndorsement.toProto();
    } catch (const std::exception&) {
        done.set_status(rsp::proto::ENDORSEMENT_FAILED);
    }

    rsp::packServiceMessage(reply, done);
    return send(reply);
}

}  // namespace rsp::endorsement_service
