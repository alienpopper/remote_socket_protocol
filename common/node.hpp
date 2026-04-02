#pragma once

#include <array>
#include <cstdint>

namespace rsp {

class RSPNode {
public:
    RSPNode();
    virtual ~RSPNode() = default;

    virtual int run() const = 0;

protected:
    const std::array<uint8_t, 16>& instanceSeed() const;

private:
    std::array<uint8_t, 16> instanceSeed_;
};

}  // namespace rsp