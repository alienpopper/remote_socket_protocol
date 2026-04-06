#pragma once

#include "common/encoding/encoding.hpp"

#include <cstdint>

namespace rsp::encoding::protobuf {

class ProtobufEncoding : public rsp::encoding::Encoding {
public:
    static constexpr uint32_t kFrameMagic = 0x52535050U;

    ProtobufEncoding(rsp::transport::ConnectionHandle connection,
                     rsp::MessageQueueHandle receivedMessages,
                     rsp::KeyPair localKeyPair);

private:
    bool readMessage(rsp::proto::RSPMessage& message) override;
    bool writeMessage(const rsp::proto::RSPMessage& message) override;
};

}  // namespace rsp::encoding::protobuf