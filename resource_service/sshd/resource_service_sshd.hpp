#pragma once

#include "resource_service/bsd_sockets/resource_service_bsd_sockets.hpp"
#include "common/base_types.hpp"

#include <string>

namespace rsp::resource_service {

// Well-known UUID for sshd name service records (the "type" field).
// Both rsp_sshd (when registering) and rsp_ssh (when querying) use this
// constant to scope name lookups to SSH service entries.
inline const rsp::GUID kSshdNameType{"c3d4e5f6-a7b8-4c9d-8e0f-1a2b3c4d5e6f"};

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

struct SshdConfig {
    std::string rspTransport;
    std::string sshdPath    = "/usr/sbin/sshd";
    std::string sshdConfig;
    bool        sshdDebug   = false;
    std::string nsHostname;   // if non-empty, register this name with NS at startup
    std::string keypairPublicKeyPath;   // if set (with keypairPrivateKeyPath), load persistent keypair
    std::string keypairPrivateKeyPath;
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

    rsp::proto::ResourceAdvertisement buildResourceAdvertisement() const override;
    bool handleNodeSpecificMessage(const rsp::proto::RSPMessage& message) override;
    bool handleConnectSshd(const rsp::proto::RSPMessage& message);

    SshdConfig cfg_;
};

}  // namespace rsp::resource_service
