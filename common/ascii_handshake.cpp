#include "common/ascii_handshake.hpp"

#include "common/base_types.hpp"
#include "common/version.h"

#include <cstdint>
#include <stdexcept>
#include <string>

namespace rsp::ascii_handshake {

namespace {

constexpr const char* kMessageTerminator = "\r\n\r\n";
constexpr size_t kMaxHandshakeBytes = 4096;

Buffer stringToBuffer(const std::string& value) {
    if (value.empty()) {
        return Buffer();
    }

    return Buffer(reinterpret_cast<const uint8_t*>(value.data()), static_cast<uint32_t>(value.size()));
}

bool sendAll(const rsp::transport::ConnectionHandle& connection, const std::string& message) {
    if (connection == nullptr) {
        return false;
    }

    const Buffer buffer = stringToBuffer(message);
    uint32_t totalSent = 0;
    while (totalSent < buffer.size()) {
        Buffer remaining(buffer.data() + totalSent, buffer.size() - totalSent);
        const int bytesSent = connection->send(remaining);
        if (bytesSent <= 0) {
            return false;
        }

        totalSent += static_cast<uint32_t>(bytesSent);
    }

    return true;
}

bool receiveMessage(const rsp::transport::ConnectionHandle& connection, std::string& message) {
    if (connection == nullptr) {
        return false;
    }

    message.clear();
    Buffer buffer(256);
    while (message.find(kMessageTerminator) == std::string::npos) {
        const int bytesRead = connection->recv(buffer);
        if (bytesRead <= 0) {
            return false;
        }

        message.append(reinterpret_cast<const char*>(buffer.data()), static_cast<size_t>(bytesRead));
        if (message.size() > kMaxHandshakeBytes) {
            return false;
        }
    }

    return true;
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

}  // namespace

std::string serverAdvertisement() {
    return std::string("RSP version ") + RSP_VERSION + "\r\n"
           "encodings:protobuf\r\n"
           "asymmetric: P256\r\n"
           "\r\n";
}

std::string clientSelection() {
    return std::string("encoding:") + kEncoding + "\r\n\r\n";
}

std::string successResponse() {
    return std::string("1success: encoding:") + kEncoding + "\r\n\r\n";
}

std::string errorResponse(const std::string& message) {
    return std::string("0error: ") + message + "\r\n\r\n";
}

bool performServerHandshake(const rsp::transport::ConnectionHandle& connection) {
    if (!sendAll(connection, serverAdvertisement())) {
        return false;
    }

    std::string clientMessage;
    if (!receiveMessage(connection, clientMessage)) {
        return false;
    }

    if (headerValue(clientMessage, "encoding:") != kEncoding) {
        sendAll(connection, errorResponse("unsupported encoding"));
        connection->close();
        return false;
    }

    if (!sendAll(connection, successResponse())) {
        return false;
    }

    return true;
}

bool performClientHandshake(const rsp::transport::ConnectionHandle& connection) {
    std::string serverMessage;
    if (!receiveMessage(connection, serverMessage)) {
        return false;
    }

    if (!sendAll(connection, clientSelection())) {
        return false;
    }

    std::string serverResult;
    if (!receiveMessage(connection, serverResult)) {
        return false;
    }

    return !serverResult.empty() && serverResult[0] == '1';
}

}  // namespace rsp::ascii_handshake