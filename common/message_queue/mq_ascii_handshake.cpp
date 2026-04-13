#include "common/message_queue/mq_ascii_handshake.hpp"

#include "common/encoding/json/json_encoding.hpp"
#include "common/encoding/protobuf/protobuf_encoding.hpp"
#include "common/version.h"

#include <optional>
#include <string>

namespace rsp::message_queue {

namespace {

constexpr const char* kMessageTerminator = "\r\n\r\n";
constexpr size_t kMaxHandshakeBytes = 4096;

using ConnectionHandle = rsp::transport::ConnectionHandle;

rsp::Buffer stringToBuffer(const std::string& value) {
    if (value.empty()) {
        return rsp::Buffer();
    }

    return rsp::Buffer(reinterpret_cast<const uint8_t*>(value.data()), static_cast<uint32_t>(value.size()));
}

bool sendAll(const ConnectionHandle& connection, const std::string& message) {
    if (connection == nullptr) {
        return false;
    }

    const rsp::Buffer buffer = stringToBuffer(message);
    return connection->sendAll(buffer.data(), buffer.size());
}

bool receiveMessage(const ConnectionHandle& connection, std::string& message) {
    if (connection == nullptr) {
        return false;
    }

    // Read one byte at a time to avoid consuming bytes beyond the \r\n\r\n
    // terminator that belong to the subsequent protobuf encoding phase.
    message.clear();
    uint8_t byte = 0;
    while (message.size() < kMaxHandshakeBytes) {
        if (!connection->readExact(&byte, 1)) {
            return false;
        }
        message += static_cast<char>(byte);
        const size_t len = message.size();
        if (len >= 4 &&
            message[len - 4] == '\r' && message[len - 3] == '\n' &&
            message[len - 2] == '\r' && message[len - 1] == '\n') {
            return true;
        }
    }

    return false;
}

std::string trim(const std::string& value) {
    size_t start = 0;
    while (start < value.size() && (value[start] == ' ' || value[start] == '\t')) {
        ++start;
    }

    size_t end = value.size();
    while (end > start && (value[end - 1] == ' ' || value[end - 1] == '\t')) {
        --end;
    }

    return value.substr(start, end - start);
}

std::string headerValue(const std::string& message, const std::string& key) {
    size_t lineStart = 0;
    while (lineStart < message.size()) {
        const size_t lineEnd = message.find("\r\n", lineStart);
        if (lineEnd == std::string::npos) {
            break;
        }

        if (lineEnd == lineStart) {
            break;
        }

        const std::string line = message.substr(lineStart, lineEnd - lineStart);
        if (line.rfind(key, 0) == 0) {
            return trim(line.substr(key.size()));
        }

        lineStart = lineEnd + 2;
    }

    return std::string();
}

std::string serverAdvertisement() {
    return std::string("RSP version ") + RSP_VERSION + "\r\n"
           "encodings:protobuf,json\r\n"
           "asymmetric: P256\r\n"
           "\r\n";
}

std::string clientSelection(const std::string& encoding) {
    return std::string("encoding:") + encoding + "\r\n\r\n";
}

std::string successResponse(const std::string& encoding) {
    return std::string("1success: encoding:") + encoding + "\r\n\r\n";
}

std::string errorResponse(const std::string& message) {
    return std::string("0error: ") + message + "\r\n\r\n";
}

std::optional<std::string> performServerHandshake(const ConnectionHandle& connection) {
    if (!sendAll(connection, serverAdvertisement())) {
        return std::nullopt;
    }

    std::string clientMessage;
    if (!receiveMessage(connection, clientMessage)) {
        return std::nullopt;
    }

    const std::string selectedEncoding = headerValue(clientMessage, "encoding:");
    if (selectedEncoding != kAsciiHandshakeEncoding && selectedEncoding != kJsonHandshakeEncoding) {
        sendAll(connection, errorResponse("unsupported encoding"));
        connection->close();
        return std::nullopt;
    }

    if (!sendAll(connection, successResponse(selectedEncoding))) {
        return std::nullopt;
    }

    return selectedEncoding;
}

std::optional<std::string> performClientHandshake(const ConnectionHandle& connection, const std::string& requestedEncoding) {
    std::string serverMessage;
    if (!receiveMessage(connection, serverMessage)) {
        return std::nullopt;
    }

    if (!sendAll(connection, clientSelection(requestedEncoding))) {
        return std::nullopt;
    }

    std::string serverResult;
    if (!receiveMessage(connection, serverResult)) {
        return std::nullopt;
    }

    if (serverResult.empty() || serverResult[0] != '1') {
        return std::nullopt;
    }

    const std::string successPrefix = "1success: encoding:";
    const size_t lineEnd = serverResult.find("\r\n");
    if (serverResult.rfind(successPrefix, 0) != 0) {
        return std::nullopt;
    }

    return serverResult.substr(successPrefix.size(),
                               lineEnd == std::string::npos ? std::string::npos : lineEnd - successPrefix.size());
}

rsp::encoding::EncodingHandle createEncodingForConnection(const ConnectionHandle& connection,
                                                          const rsp::MessageQueueHandle& receivedMessages,
                                                          const rsp::KeyPair& localKeyPair) {
    if (connection == nullptr) {
        return nullptr;
    }

    const auto selectedEncoding = connection->negotiatedEncoding();
    if (!selectedEncoding.has_value()) {
        return nullptr;
    }

    if (*selectedEncoding == kAsciiHandshakeEncoding) {
        return std::make_shared<rsp::encoding::protobuf::ProtobufEncoding>(connection,
                                                                           receivedMessages,
                                                                           localKeyPair.duplicate());
    }

    if (*selectedEncoding == kJsonHandshakeEncoding) {
        return std::make_shared<rsp::encoding::json::JsonEncoding>(connection,
                                                                   receivedMessages,
                                                                   localKeyPair.duplicate());
    }

    return nullptr;
}

}  // namespace

MessageQueueAsciiHandshakeServer::MessageQueueAsciiHandshakeServer(rsp::MessageQueueHandle receivedMessages,
                                                                   rsp::KeyPair localKeyPair,
                                                                   SuccessCallback success,
                                                                   FailureCallback failure)
    : receivedMessages_(std::move(receivedMessages)),
      localKeyPair_(std::move(localKeyPair)),
      success_(std::move(success)),
      failure_(std::move(failure)) {
}

void MessageQueueAsciiHandshakeServer::handleMessage(Message transport, rsp::MessageQueueSharedState&) {
    if (transport == nullptr) {
        return;
    }

    const auto selectedEncoding = performServerHandshake(transport);
    if (!selectedEncoding.has_value()) {
        if (failure_) {
            failure_(transport);
        }
        transport->close();
        return;
    }

    transport->setNegotiatedEncoding(*selectedEncoding);
    const auto newEncoding = createEncodingForConnection(transport, receivedMessages_, localKeyPair_);
    if (newEncoding == nullptr) {
        if (failure_) {
            failure_(transport);
        }
        transport->close();
        return;
    }

    if (success_) {
        success_(newEncoding);
    }
}

void MessageQueueAsciiHandshakeServer::handleQueueFull(size_t, size_t, const Message& transport) {
    if (failure_) {
        failure_(transport);
    }
    if (transport != nullptr) {
        transport->close();
    }
}

MessageQueueAsciiHandshakeClient::MessageQueueAsciiHandshakeClient(rsp::MessageQueueHandle receivedMessages,
                                                                   rsp::KeyPair localKeyPair,
                                                                   std::string requestedEncoding,
                                                                   SuccessCallback success,
                                                                   FailureCallback failure)
    : receivedMessages_(std::move(receivedMessages)),
      localKeyPair_(std::move(localKeyPair)),
      requestedEncoding_(std::move(requestedEncoding)),
      success_(std::move(success)),
      failure_(std::move(failure)) {
}

void MessageQueueAsciiHandshakeClient::handleMessage(Message transport, rsp::MessageQueueSharedState&) {
    if (transport == nullptr) {
        return;
    }

    const auto connection = transport->connection();
    if (connection == nullptr) {
        if (failure_) {
            failure_(transport);
        }
        transport->stop();
        return;
    }

    const auto selectedEncoding = performClientHandshake(connection, requestedEncoding_);
    if (!selectedEncoding.has_value() || *selectedEncoding != requestedEncoding_) {
        if (failure_) {
            failure_(transport);
        }
        transport->stop();
        return;
    }

    connection->setNegotiatedEncoding(*selectedEncoding);
    const auto newEncoding = createEncodingForConnection(connection, receivedMessages_, localKeyPair_);
    if (newEncoding == nullptr) {
        if (failure_) {
            failure_(transport);
        }
        transport->stop();
        return;
    }

    if (success_) {
        success_(newEncoding);
    }
}

void MessageQueueAsciiHandshakeClient::handleQueueFull(size_t, size_t, const Message& transport) {
    if (failure_) {
        failure_(transport);
    }
    if (transport != nullptr) {
        transport->stop();
    }
}

}  // namespace rsp::message_queue