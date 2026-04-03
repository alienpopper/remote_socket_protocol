#pragma once

#include "common/transport/transport.hpp"

#include <string>

namespace rsp::ascii_handshake {

constexpr const char* kEncoding = "protobuf";
constexpr const char* kAsymmetricAlgorithm = "P256";

std::string serverAdvertisement();
std::string clientSelection();
std::string successResponse();
std::string errorResponse(const std::string& message);

bool performServerHandshake(const rsp::transport::ConnectionHandle& connection);
bool performClientHandshake(const rsp::transport::ConnectionHandle& connection);

}  // namespace rsp::ascii_handshake