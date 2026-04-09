#pragma once

#include "client/cpp_full/rsp_client.hpp"

#include <memory>

namespace rsp::endorsement_service {

class EndorsementService : public rsp::client::full::RSPClient {
public:
    using Ptr = std::shared_ptr<EndorsementService>;
    using ConstPtr = std::shared_ptr<const EndorsementService>;

    static Ptr create();
    static Ptr create(KeyPair keyPair);

    ~EndorsementService() override = default;

    EndorsementService(const EndorsementService&) = delete;
    EndorsementService& operator=(const EndorsementService&) = delete;
    EndorsementService(EndorsementService&&) = delete;
    EndorsementService& operator=(EndorsementService&&) = delete;

protected:
    explicit EndorsementService(KeyPair keyPair);

    bool handleNodeSpecificMessage(const rsp::proto::RSPMessage& message) override;
    bool handleBeginEndorsementRequest(const rsp::proto::RSPMessage& message);
};

}  // namespace rsp::endorsement_service
