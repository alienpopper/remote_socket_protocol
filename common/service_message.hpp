#pragma once

#include "messages.pb.h"

#include <google/protobuf/any.pb.h>

#include <string>

namespace rsp {

// Type URL prefix for RSP service messages packed into google.protobuf.Any.
constexpr const char* kTypeUrlPrefix = "type.rsp/";

// Pack a service-specific protobuf message into the service_message Any field
// of an RSPMessage envelope.
template <typename T>
void packServiceMessage(rsp::proto::RSPMessage& envelope, const T& serviceMsg) {
    envelope.mutable_service_message()->PackFrom(serviceMsg, kTypeUrlPrefix);
}

// Unpack a service-specific protobuf message from the service_message Any field.
// Returns true if the Any type_url matches and deserialization succeeds.
template <typename T>
bool unpackServiceMessage(const rsp::proto::RSPMessage& envelope, T* serviceMsg) {
    if (!envelope.has_service_message()) {
        return false;
    }
    const std::string expectedUrl = std::string(kTypeUrlPrefix) + T::descriptor()->full_name();
    if (envelope.service_message().type_url() != expectedUrl) {
        return false;
    }
    return envelope.service_message().UnpackTo(serviceMsg);
}

// Check if the service_message Any contains a specific message type.
template <typename T>
bool hasServiceMessage(const rsp::proto::RSPMessage& envelope) {
    if (!envelope.has_service_message()) {
        return false;
    }
    const std::string expectedUrl = std::string(kTypeUrlPrefix) + T::descriptor()->full_name();
    return envelope.service_message().type_url() == expectedUrl;
}

// Extract just the type name (after the last '/') from a type_url.
inline std::string typeNameFromUrl(const std::string& typeUrl) {
    const auto pos = typeUrl.rfind('/');
    if (pos == std::string::npos) {
        return typeUrl;
    }
    return typeUrl.substr(pos + 1);
}

}  // namespace rsp
