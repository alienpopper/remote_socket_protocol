#include "endorsement_service/endorsement_service.hpp"

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

bool EndorsementService::handleNodeSpecificMessage(const rsp::proto::RSPMessage& /*message*/) {
    // No endorsement-specific messages handled yet.
    return false;
}

}  // namespace rsp::endorsement_service
