#include "common/node.hpp"

#include "os/os_random.hpp"

namespace rsp {

RSPNode::RSPNode() {
    rsp::os::randomFill(instanceSeed_.data(), static_cast<uint32_t>(instanceSeed_.size()));
}

const std::array<uint8_t, 16>& RSPNode::instanceSeed() const {
    return instanceSeed_;
}

}  // namespace rsp