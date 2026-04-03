#pragma once

#include "common/transport/transport.hpp"

#include <optional>
#include <string>

namespace rsp::ascii_handshake {

constexpr const char* kEncoding = "protobuf";
constexpr const char* kAsymmetricAlgorithm = "P256";

std::string serverAdvertisement();
std::string clientSelection();
std::string successResponse();
std::string errorResponse(const std::string& message);

std::optional<std::string> performServerHandshake(const rsp::transport::ConnectionHandle& connection);
std::optional<std::string> performClientHandshake(const rsp::transport::ConnectionHandle& connection);

}  // namespace rsp::ascii_handshake