#include "common/encoding/protobuf/protobuf_encoding.hpp"

#include "common/base_types.hpp"
#include "common/ping_trace.hpp"

#include <cstdint>
#include <cstring>
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

std::string toProtoNodeIdValue(const rsp::NodeID& nodeId) {
    std::string value(16, '\0');
    const uint64_t high = nodeId.high();
    const uint64_t low = nodeId.low();
    std::memcpy(value.data(), &high, sizeof(high));
    std::memcpy(value.data() + sizeof(high), &low, sizeof(low));
    return value;
}

std::string classifySendPrefix(const rsp::proto::RSPMessage& message, const rsp::KeyPair& localKeyPair) {
    const std::string localNodeId = toProtoNodeIdValue(localKeyPair.nodeID());

    if (message.has_ping_request()) {
        if (message.source().value() == localNodeId) {
            return "source_request";
        }

        return "rm_forward_request";
    }

    if (message.has_ping_reply() && message.source().value() == localNodeId) {
        return "destination_reply";
    }

    return "";
}

void recordSendEvent(const rsp::proto::RSPMessage& message,
                     const rsp::KeyPair& localKeyPair,
                     const std::string& suffix) {
    if (!rsp::ping_trace::isEnabled()) {
        return;
    }

    const std::string prefix = classifySendPrefix(message, localKeyPair);
    if (!prefix.empty()) {
        rsp::ping_trace::recordForMessage(message, prefix + "_" + suffix);
    }
}

}  // namespace

ProtobufEncoding::ProtobufEncoding(rsp::transport::ConnectionHandle connection,
                                   rsp::MessageQueueHandle receivedMessages,
                                   rsp::KeyPair localKeyPair)
    : Encoding(std::move(connection), std::move(receivedMessages), std::move(localKeyPair)) {
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
    recordSendEvent(message, localKeyPair(), "serialize_start");
    if (!message.SerializeToString(&payload) || payload.size() > kMaxFrameLength) {
        return false;
    }
    recordSendEvent(message, localKeyPair(), "serialize_done");

    std::string header;
    header.reserve(kFrameHeaderSize);
    appendUint32(header, kFrameMagic);
    appendUint32(header, static_cast<uint32_t>(payload.size()));

    const bool sent = activeConnection->sendAll(reinterpret_cast<const uint8_t*>(header.data()), static_cast<uint32_t>(header.size())) &&
                      activeConnection->sendAll(reinterpret_cast<const uint8_t*>(payload.data()), static_cast<uint32_t>(payload.size()));
    if (sent) {
        recordSendEvent(message, localKeyPair(), "transport_send_done");
    }

    return sent;
}

}  // namespace rsp::encoding::protobuf