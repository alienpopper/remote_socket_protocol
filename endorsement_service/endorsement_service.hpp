#pragma once

#include "client/cpp_full/rsp_client.hpp"
#include "common/endorsement/endorsement.hpp"

#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace rsp::endorsement_service {

class EndorsementService : public rsp::client::full::RSPClient {
public:
    using Ptr = std::shared_ptr<EndorsementService>;
    using ConstPtr = std::shared_ptr<const EndorsementService>;

    struct ConfiguredEndorsement {
        std::optional<rsp::NodeID> requestor;
        rsp::GUID endorsementType;
        rsp::Buffer endorsementValue;
        double validForSeconds = DAYS(1);
    };

    static Ptr create();
    static Ptr create(KeyPair keyPair);

    ~EndorsementService() override = default;

    EndorsementService(const EndorsementService&) = delete;
    EndorsementService& operator=(const EndorsementService&) = delete;
    EndorsementService(EndorsementService&&) = delete;
    EndorsementService& operator=(EndorsementService&&) = delete;

    void setConfiguredEndorsements(std::vector<ConfiguredEndorsement> configuredEndorsements);
    size_t configuredEndorsementCount() const;

protected:
    explicit EndorsementService(KeyPair keyPair);

    bool handleNodeSpecificMessage(const rsp::proto::RSPMessage& message) override;
    bool handleBeginEndorsementRequest(const rsp::proto::RSPMessage& message);

private:
    struct ConfiguredEndorsementMatch {
        bool hasConfiguredEndorsements = false;
        std::optional<ConfiguredEndorsement> endorsement;
    };

    ConfiguredEndorsementMatch findMatchingConfiguredEndorsement(
        const rsp::NodeID& requestorNodeId,
        const rsp::Endorsement& requestedValues) const;

    mutable std::mutex configuredEndorsementsMutex_;
    std::vector<ConfiguredEndorsement> configuredEndorsements_;
};

}  // namespace rsp::endorsement_service
