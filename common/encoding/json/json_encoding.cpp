#include "common/encoding/json/json_encoding.hpp"

#include "common/message_queue/mq_signing.hpp"
#include "common/service_message.hpp"

#include <google/protobuf/util/json_util.h>

#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>

namespace rsp::encoding::json {

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

std::string encodeMessage(const rsp::proto::RSPMessage& message) {
    google::protobuf::util::JsonPrintOptions options;
    options.preserve_proto_field_names = true;
    options.always_print_enums_as_ints = true;
    std::string output;
    auto status = google::protobuf::util::MessageToJsonString(message, &output, options);
    if (!status.ok()) {
        std::cerr << "[json_encoding] MessageToJsonString failed: " << status.message() << std::endl;
        return "{}";
    }
    return output;
}

bool decodeMessage(const std::string& payload, rsp::proto::RSPMessage& message) {
    google::protobuf::util::JsonParseOptions options;
    options.case_insensitive_enum_parsing = true;
    auto status = google::protobuf::util::JsonStringToMessage(payload, &message, options);
    if (!status.ok()) {
        std::cerr << "[json_encoding] JsonStringToMessage failed: " << status.message() << std::endl;
    }
    return status.ok();
}

std::string messageKind(const rsp::proto::RSPMessage& message) {
    if (message.has_ping_request()) return "ping_request";
    if (message.has_ping_reply()) return "ping_reply";
    if (message.has_identity()) return "identity";
    if (message.has_challenge_request()) return "challenge_request";
    if (message.has_endorsement_needed()) return "endorsement_needed";
    if (message.has_resource_query()) return "resource_query";
    if (message.has_resource_advertisement()) return "resource_advertisement";
    if (message.has_error()) return "error";
    if (message.has_service_message()) return rsp::typeNameFromUrl(message.service_message().type_url());
    return "unknown";
}

}  // namespace

JsonEncoding::JsonEncoding(rsp::transport::ConnectionHandle connection,
                           rsp::MessageQueueHandle receivedMessages,
                           rsp::KeyPair localKeyPair)
    : Encoding(std::move(connection), std::move(receivedMessages), std::move(localKeyPair)) {
}

bool JsonEncoding::readMessage(rsp::proto::RSPMessage& message) {
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
        std::cerr << "[json_encoding] read failure: bad magic=0x" << std::hex << magic << std::dec << std::endl;
        return false;
    }

    const uint32_t payloadLength = readUint32(header + 4);
    if (payloadLength > kMaxFrameLength) {
        std::cerr << "[json_encoding] read failure: payload too large bytes=" << payloadLength << std::endl;
        return false;
    }

    std::string payload(payloadLength, '\0');
    if (!activeConnection->readExact(reinterpret_cast<uint8_t*>(payload.data()), payloadLength)) {
        std::cerr << "[json_encoding] read failure: payload readExact failed bytes=" << payloadLength << std::endl;
        return false;
    }

    if (!decodeMessage(payload, message)) {
        std::cerr << "[json_encoding] read failure: decodeMessage failed bytes=" << payloadLength << std::endl;
        return false;
    }

    if (rsp::messageTraceEnabled(message)) {
        std::cerr << "[json_encoding] read success kind=" << messageKind(message)
                  << " bytes=" << payloadLength << std::endl;
    }
    return true;
}

bool JsonEncoding::writeMessage(const rsp::proto::RSPMessage& message) {
    const rsp::transport::ConnectionHandle activeConnection = connection();
    if (activeConnection == nullptr) {
        return false;
    }

    const std::string payload = encodeMessage(message);
    const bool trace = rsp::messageTraceEnabled(message);
    if (payload.size() > kMaxFrameLength) {
        if (trace) {
            std::cerr << "[json_encoding] write failure: payload too large bytes=" << payload.size()
                      << " kind=" << messageKind(message) << std::endl;
        }
        return false;
    }

    std::string header;
    header.reserve(kFrameHeaderSize);
    appendUint32(header, kFrameMagic);
    appendUint32(header, static_cast<uint32_t>(payload.size()));

    const bool headerSent = activeConnection->sendAll(reinterpret_cast<const uint8_t*>(header.data()), static_cast<uint32_t>(header.size()));
    const bool payloadSent = headerSent &&
                             activeConnection->sendAll(reinterpret_cast<const uint8_t*>(payload.data()), static_cast<uint32_t>(payload.size()));
    if (!headerSent || !payloadSent) {
        if (trace) {
            std::cerr << "[json_encoding] write failure: sendAll failed kind=" << messageKind(message)
                      << " bytes=" << payload.size() << std::endl;
        }
        return false;
    }

    if (trace) {
        std::cerr << "[json_encoding] write success kind=" << messageKind(message)
                  << " bytes=" << payload.size() << std::endl;
    }
    return true;
}

}  // namespace rsp::encoding::json

