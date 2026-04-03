#pragma once

#include <array>
#include <cstdint>

#include "common/keypair.hpp"
#include "common/message_queue.hpp"

namespace rsp {

class NodeInputQueue;

class RSPNode {
public:
    RSPNode();
    explicit RSPNode(KeyPair keyPair);
    virtual ~RSPNode();

    virtual int run() const = 0;

    bool enqueueInput(rsp::proto::RSPMessage message) const;
    size_t pendingInputCount() const;
    size_t pendingOutputCount() const;

protected:
    virtual bool handleNodeSpecificMessage(const rsp::proto::RSPMessage& message) = 0;
    virtual void handleOutputMessage(rsp::proto::RSPMessage message) = 0;

    const std::array<uint8_t, 16>& instanceSeed() const;
    const KeyPair& keyPair() const;
    rsp::MessageQueueHandle inputQueue() const;
    rsp::MessageQueueHandle outputQueue() const;

private:
    friend class NodeInputQueue;
    friend class NodeOutputQueue;

    KeyPair keyPair_;
    std::array<uint8_t, 16> instanceSeed_;
    rsp::MessageQueueHandle inputQueue_;
    rsp::MessageQueueHandle outputQueue_;
};

}  // namespace rsp