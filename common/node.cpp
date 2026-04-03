#include "common/node.hpp"

#include "os/os_random.hpp"

#include <utility>

namespace rsp {

RSPNode::RSPNode() : RSPNode(KeyPair::generateP256()) {
}

RSPNode::RSPNode(KeyPair keyPair) : keyPair_(std::move(keyPair)) {
    rsp::os::randomFill(instanceSeed_.data(), static_cast<uint32_t>(instanceSeed_.size()));
}

const std::array<uint8_t, 16>& RSPNode::instanceSeed() const {
    return instanceSeed_;
}

const KeyPair& RSPNode::keyPair() const {
    return keyPair_;
}

}  // namespace rsp