#pragma once

#include <array>
#include <cstdint>

#include "common/keypair.hpp"

namespace rsp {

class RSPNode {
public:
    RSPNode();
    explicit RSPNode(KeyPair keyPair);
    virtual ~RSPNode() = default;

    virtual int run() const = 0;

protected:
    const std::array<uint8_t, 16>& instanceSeed() const;
    const KeyPair& keyPair() const;

private:
    KeyPair keyPair_;
    std::array<uint8_t, 16> instanceSeed_;
};

}  // namespace rsp