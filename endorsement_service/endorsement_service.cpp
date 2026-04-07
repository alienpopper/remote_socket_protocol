#include "endorsement_service/endorsement_service.hpp"

#include "common/endorsement/endorsement.hpp"
#include "common/endorsement/well_known_endorsements.h"

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

rsp::Buffer stringToBuffer(const std::string& value) {
    if (value.empty()) {
        return rsp::Buffer();
    }

    return rsp::Buffer(reinterpret_cast<const uint8_t*>(value.data()), static_cast<uint32_t>(value.size()));
}

rsp::Buffer serializeEndorsementMessage(const rsp::proto::Endorsement& message) {
    std::string serialized;
    if (!message.SerializeToString(&serialized)) {
        throw std::runtime_error("failed to serialize endorsement request");
    }

    return stringToBuffer(serialized);
}

rsp::proto::Endorsement toProtoEndorsement(const rsp::Endorsement& endorsement) {
    rsp::proto::Endorsement message;
    const rsp::Buffer serialized = endorsement.serialize();
    if (!message.ParseFromArray(serialized.data(), static_cast<int>(serialized.size()))) {
        throw std::runtime_error("failed to parse signed endorsement");
    }

    return message;
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

bool EndorsementService::handleNodeSpecificMessage(const rsp::proto::RSPMessage& message) {
    if (message.has_identity()) {
        return true;
    }

    if (message.has_begin_endorsement_request()) {
        return handleBeginEndorsementRequest(message);
    }

    return false;
}

bool EndorsementService::handleBeginEndorsementRequest(const rsp::proto::RSPMessage& message) {
    rsp::proto::RSPMessage reply;
    *reply.mutable_source() = toProtoNodeId(keyPair().nodeID());
    if (message.has_source()) {
        *reply.mutable_destination() = message.source();
    }

    auto* done = reply.mutable_endorsement_done();
    done->set_status(rsp::proto::ENDORSEMENT_FAILED);

    try {
        if (!message.has_source()) {
            return send(reply);
        }

        const auto sourceNodeId = fromProtoNodeId(message.source());
        if (!sourceNodeId.has_value()) {
            done->set_status(rsp::proto::ENDORSEMENT_INVALID_SIGNATURE);
            return send(reply);
        }

        const auto cachedIdentity = identityCache().get(*sourceNodeId);
        if (!cachedIdentity.has_value()) {
            done->set_status(rsp::proto::ENDORSEMENT_UNKNOWN_IDENTITY);
            return send(reply);
        }

        const auto& request = message.begin_endorsement_request();
        if (!request.has_requested_values()) {
            done->set_status(rsp::proto::ENDORSEMENT_INVALID_SIGNATURE);
            return send(reply);
        }

        const rsp::KeyPair requesterPublicKey = rsp::KeyPair::fromPublicKey(cachedIdentity->public_key());
        if (requesterPublicKey.nodeID() != *sourceNodeId) {
            done->set_status(rsp::proto::ENDORSEMENT_INVALID_SIGNATURE);
            return send(reply);
        }

        const rsp::Endorsement requestedValues = rsp::Endorsement::deserialize(
            serializeEndorsementMessage(request.requested_values()));
        if (requestedValues.subject() != *sourceNodeId ||
            requestedValues.endorsementService() != *sourceNodeId ||
            !requestedValues.verifySignature(requesterPublicKey)) {
            done->set_status(rsp::proto::ENDORSEMENT_INVALID_SIGNATURE);
            return send(reply);
        }

        rsp::DateTime validUntil;
        validUntil += DAYS(1);
        const rsp::Endorsement issuedEndorsement = rsp::Endorsement::createSigned(
            keyPair(),
            requestedValues.subject(),
            requestedValues.endorsementType(),
            requestedValues.endorsementValue(),
            validUntil);

        done->set_status(rsp::proto::ENDORSEMENT_SUCCESS);
        *done->mutable_new_endorsement() = toProtoEndorsement(issuedEndorsement);
    } catch (const std::exception&) {
        done->set_status(rsp::proto::ENDORSEMENT_FAILED);
    }

    return send(reply);
}

}  // namespace rsp::endorsement_service
