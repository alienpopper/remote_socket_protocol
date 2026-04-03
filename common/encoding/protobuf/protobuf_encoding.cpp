#include "common/encoding/protobuf/protobuf_encoding.hpp"

#include "common/base_types.hpp"

#include <cstdint>
#include <string>

namespace rsp::encoding::protobuf {

namespace {

constexpr uint32_t kFrameHeaderSize = 8;
constexpr uint32_t kMaxFrameLength = 16U * 1024U * 1024U;

void appendUint32(std::string& buffer, uint32_t value) {
    buffer.push_back(static_cast<char>((value >> 24) & 0xFFU));
    buffer.push_back(static_cast<char>((value >> 16) & 0xFFU));
    buffer.push_back(static_cast<char>((value >> 8) & 0xFFU));
    buffer.push_back(static_cast<char>(value & 0xFFU));
}

uint32_t readUint32(const uint8_t* data) {
    return (static_cast<uint32_t>(data[0]) << 24) |
           (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) |
           static_cast<uint32_t>(data[3]);
}

}  // namespace

ProtobufEncoding::ProtobufEncoding(rsp::transport::ConnectionHandle connection,
                                   rsp::MessageQueueHandle receivedMessages,
                                   const rsp::KeyPair& localKeyPair)
    : Encoding(std::move(connection), std::move(receivedMessages), localKeyPair) {
}

bool ProtobufEncoding::readMessage(rsp::proto::RSPMessage& message) {
    const rsp::transport::ConnectionHandle activeConnection = connection();
    if (activeConnection == nullptr) {
        return false;
    }

    uint8_t header[kFrameHeaderSize] = {};
    if (!activeConnection->readExact(header, kFrameHeaderSize)) {
        return false;
    }

    const uint32_t magic = readUint32(header);
    if (magic != kFrameMagic) {
        return false;
    }

    const uint32_t payloadLength = readUint32(header + 4);
    if (payloadLength > kMaxFrameLength) {
        return false;
    }

    std::string payload(payloadLength, '\0');
    if (!activeConnection->readExact(reinterpret_cast<uint8_t*>(payload.data()), payloadLength)) {
        return false;
    }

    return message.ParseFromArray(payload.data(), static_cast<int>(payload.size()));
}

bool ProtobufEncoding::writeMessage(const rsp::proto::RSPMessage& message) {
    const rsp::transport::ConnectionHandle activeConnection = connection();
    if (activeConnection == nullptr) {
        return false;
    }

    std::string payload;
    if (!message.SerializeToString(&payload) || payload.size() > kMaxFrameLength) {
        return false;
    }

    std::string header;
    header.reserve(kFrameHeaderSize);
    appendUint32(header, kFrameMagic);
    appendUint32(header, static_cast<uint32_t>(payload.size()));

    return activeConnection->sendAll(reinterpret_cast<const uint8_t*>(header.data()), static_cast<uint32_t>(header.size())) &&
           activeConnection->sendAll(reinterpret_cast<const uint8_t*>(payload.data()), static_cast<uint32_t>(payload.size()));
}

}  // namespace rsp::encoding::protobuf