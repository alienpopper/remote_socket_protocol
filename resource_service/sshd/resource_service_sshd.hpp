#pragma once

#include "resource_service/bsd_sockets/resource_service_bsd_sockets.hpp"

#include <string>

namespace rsp::resource_service {

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

struct SshdConfig {
    std::string rspTransport;
    std::string sshdPath    = "/usr/sbin/sshd";
    std::string sshdConfig;
    bool        sshdDebug   = false;
};

SshdConfig loadSshdConfig(const std::string& path);

// ---------------------------------------------------------------------------
// SshdResourceService
// ---------------------------------------------------------------------------

class SshdResourceService : public BsdSocketsResourceService {
public:
    static std::shared_ptr<SshdResourceService> create(const SshdConfig& cfg);

private:
    SshdResourceService(rsp::KeyPair keyPair, const SshdConfig& cfg);

    bool handleNodeSpecificMessage(const rsp::proto::RSPMessage& message) override;
    bool handleConnectSshd(const rsp::proto::RSPMessage& message);

    SshdConfig cfg_;
};

}  // namespace rsp::resource_service
