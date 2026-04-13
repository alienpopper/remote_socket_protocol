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

protected:
    TCPConnectionResult createTCPConnection(const std::string& hostPort,
                                            uint32_t totalAttempts,
                                            uint32_t retryDelayMs) override;

    bool handleListenTCPRequest(const rsp::proto::RSPMessage& message) override;

private:
    SshdResourceService(rsp::KeyPair keyPair, const SshdConfig& cfg);

    SshdConfig cfg_;
};

}  // namespace rsp::resource_service
