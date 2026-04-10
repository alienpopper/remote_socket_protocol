#include "common/encoding/json/json_encoding.hpp"

#include "third_party/json/single_include/nlohmann/json.hpp"

#include "common/base_types.hpp"

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace rsp::encoding::json {

using nlohmann::json;

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

char hexDigit(uint8_t nibble) {
    return static_cast<char>(nibble < 10 ? ('0' + nibble) : ('a' + (nibble - 10)));
}

std::string encodeBytes(const std::string& bytes) {
    std::string encoded;
    encoded.reserve(bytes.size() * 2);
    for (unsigned char byte : bytes) {
        encoded.push_back(hexDigit(static_cast<uint8_t>(byte >> 4)));
        encoded.push_back(hexDigit(static_cast<uint8_t>(byte & 0x0F)));
    }
    return encoded;
}

int decodeHexNibble(char value) {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return 10 + (value - 'a');
    }
    if (value >= 'A' && value <= 'F') {
        return 10 + (value - 'A');
    }
    return -1;
}

bool decodeBytes(const json& value, std::string& out) {
    if (!value.is_string()) {
        return false;
    }

    const std::string encoded = value.get<std::string>();
    if ((encoded.size() % 2) != 0) {
        return false;
    }

    out.clear();
    out.reserve(encoded.size() / 2);
    for (size_t index = 0; index < encoded.size(); index += 2) {
        const int high = decodeHexNibble(encoded[index]);
        const int low = decodeHexNibble(encoded[index + 1]);
        if (high < 0 || low < 0) {
            return false;
        }

        out.push_back(static_cast<char>((high << 4) | low));
    }

    return true;
}

template <typename EnumType>
json encodeEnum(EnumType value) {
    return static_cast<int>(value);
}

template <typename EnumType>
bool decodeEnum(const json& value, EnumType& out) {
    if (!value.is_number_integer()) {
        return false;
    }

    out = static_cast<EnumType>(value.get<int>());
    return true;
}

json toJson(const rsp::proto::Uuid& message) {
    return json{{"value", encodeBytes(message.value())}};
}

bool fromJson(const json& value, rsp::proto::Uuid& message) {
    if (!value.is_object()) {
        return false;
    }

    std::string bytes;
    if (!value.contains("value") || !decodeBytes(value.at("value"), bytes)) {
        return false;
    }

    message.set_value(bytes);
    return true;
}

json toJson(const rsp::proto::NodeId& message) {
    return json{{"value", encodeBytes(message.value())}};
}

bool fromJson(const json& value, rsp::proto::NodeId& message) {
    if (!value.is_object()) {
        return false;
    }

    std::string bytes;
    if (!value.contains("value") || !decodeBytes(value.at("value"), bytes)) {
        return false;
    }

    message.set_value(bytes);
    return true;
}

json toJson(const rsp::proto::SocketID& message) {
    return json{{"value", encodeBytes(message.value())}};
}

bool fromJson(const json& value, rsp::proto::SocketID& message) {
    if (!value.is_object()) {
        return false;
    }

    std::string bytes;
    if (!value.contains("value") || !decodeBytes(value.at("value"), bytes)) {
        return false;
    }

    message.set_value(bytes);
    return true;
}

json toJson(const rsp::proto::EndorsementType& message) {
    return json{{"value", encodeBytes(message.value())}};
}

bool fromJson(const json& value, rsp::proto::EndorsementType& message) {
    if (!value.is_object()) {
        return false;
    }

    std::string bytes;
    if (!value.contains("value") || !decodeBytes(value.at("value"), bytes)) {
        return false;
    }

    message.set_value(bytes);
    return true;
}

json toJson(const rsp::proto::DateTime& message) {
    return json{{"milliseconds_since_epoch", message.milliseconds_since_epoch()}};
}

bool fromJson(const json& value, rsp::proto::DateTime& message) {
    if (!value.is_object() || !value.contains("milliseconds_since_epoch") ||
        !value.at("milliseconds_since_epoch").is_number_unsigned()) {
        return false;
    }

    message.set_milliseconds_since_epoch(value.at("milliseconds_since_epoch").get<uint64_t>());
    return true;
}

json toJson(const rsp::proto::SignatureBlock& message) {
    return json{{"signer", toJson(message.signer())},
                {"algorithm", encodeEnum(message.algorithm())},
                {"signature", encodeBytes(message.signature())}};
}

bool fromJson(const json& value, rsp::proto::SignatureBlock& message) {
    if (!value.is_object() || !value.contains("signer") || !value.contains("algorithm") || !value.contains("signature")) {
        return false;
    }

    rsp::proto::SIGNATURE_ALGORITHMS algorithm = rsp::proto::SIGNATURE_ALGORITHM_UNSPECIFIED;
    std::string signature;
    if (!fromJson(value.at("signer"), *message.mutable_signer()) ||
        !decodeEnum(value.at("algorithm"), algorithm) ||
        !decodeBytes(value.at("signature"), signature)) {
        return false;
    }

    message.set_algorithm(algorithm);
    message.set_signature(signature);
    return true;
}

json toJson(const rsp::proto::PublicKey& message) {
    return json{{"algorithm", encodeEnum(message.algorithm())},
                {"public_key", encodeBytes(message.public_key())}};
}

bool fromJson(const json& value, rsp::proto::PublicKey& message) {
    if (!value.is_object() || !value.contains("algorithm") || !value.contains("public_key")) {
        return false;
    }

    rsp::proto::SIGNATURE_ALGORITHMS algorithm = rsp::proto::SIGNATURE_ALGORITHM_UNSPECIFIED;
    std::string publicKey;
    if (!decodeEnum(value.at("algorithm"), algorithm) || !decodeBytes(value.at("public_key"), publicKey)) {
        return false;
    }

    message.set_algorithm(algorithm);
    message.set_public_key(publicKey);
    return true;
}

json toJson(const rsp::proto::Endorsement& message) {
    return json{{"subject", toJson(message.subject())},
                {"endorsement_service", toJson(message.endorsement_service())},
                {"endorsement_type", toJson(message.endorsement_type())},
                {"endorsement_value", encodeBytes(message.endorsement_value())},
                {"valid_until", toJson(message.valid_until())},
                {"signature", encodeBytes(message.signature())}};
}

bool fromJson(const json& value, rsp::proto::Endorsement& message) {
    if (!value.is_object() || !value.contains("subject") || !value.contains("endorsement_service") ||
        !value.contains("endorsement_type") || !value.contains("endorsement_value") || !value.contains("valid_until") ||
        !value.contains("signature")) {
        return false;
    }

    std::string endorsementValue;
    std::string signature;
    if (!fromJson(value.at("subject"), *message.mutable_subject()) ||
        !fromJson(value.at("endorsement_service"), *message.mutable_endorsement_service()) ||
        !fromJson(value.at("endorsement_type"), *message.mutable_endorsement_type()) ||
        !decodeBytes(value.at("endorsement_value"), endorsementValue) ||
        !fromJson(value.at("valid_until"), *message.mutable_valid_until()) ||
        !decodeBytes(value.at("signature"), signature)) {
        return false;
    }

    message.set_endorsement_value(endorsementValue);
    message.set_signature(signature);
    return true;
}

json toJson(const rsp::proto::Identity& message) {
    json value{{"public_key", toJson(message.public_key())}};
    if (message.has_nonce()) {
        value["nonce"] = toJson(message.nonce());
    }
    return value;
}

bool fromJson(const json& value, rsp::proto::Identity& message) {
    if (!value.is_object() || !value.contains("public_key") ||
        !fromJson(value.at("public_key"), *message.mutable_public_key())) {
        return false;
    }

    if (value.contains("nonce") && !fromJson(value.at("nonce"), *message.mutable_nonce())) {
        return false;
    }

    return true;
}

json toJson(const rsp::proto::ChallengeRequest& message) {
    json value = json::object();
    if (message.has_nonce()) {
        value["nonce"] = toJson(message.nonce());
    }
    return value;
}

bool fromJson(const json& value, rsp::proto::ChallengeRequest& message) {
    if (!value.is_object()) {
        return false;
    }

    if (value.contains("nonce") && !fromJson(value.at("nonce"), *message.mutable_nonce())) {
        return false;
    }

    return true;
}

json toJson(const rsp::proto::RouteEntry& message) {
    return json{{"node_id", toJson(message.node_id())},
                {"hops_away", message.hops_away()},
                {"last_seen", toJson(message.last_seen())}};
}

bool fromJson(const json& value, rsp::proto::RouteEntry& message) {
    if (!value.is_object() || !value.contains("node_id") || !value.contains("hops_away") || !value.contains("last_seen") ||
        !value.at("hops_away").is_number_unsigned()) {
        return false;
    }

    if (!fromJson(value.at("node_id"), *message.mutable_node_id()) ||
        !fromJson(value.at("last_seen"), *message.mutable_last_seen())) {
        return false;
    }

    message.set_hops_away(value.at("hops_away").get<uint32_t>());
    return true;
}

json toJson(const rsp::proto::RouteUpdate& message) {
    json entries = json::array();
    for (const auto& entry : message.entries()) {
        entries.push_back(toJson(entry));
    }
    return json{{"entries", std::move(entries)}};
}

bool fromJson(const json& value, rsp::proto::RouteUpdate& message) {
    if (!value.is_object() || !value.contains("entries") || !value.at("entries").is_array()) {
        return false;
    }

    for (const auto& entry : value.at("entries")) {
        if (!fromJson(entry, *message.add_entries())) {
            return false;
        }
    }

    return true;
}

json toJson(const rsp::proto::Address& message) {
    json value = json::object();
    if (message.ipv4() != 0) {
        value["ipv4"] = message.ipv4();
    }
    if (!message.ipv6().empty()) {
        value["ipv6"] = encodeBytes(message.ipv6());
    }
    return value;
}

bool fromJson(const json& value, rsp::proto::Address& message) {
    if (!value.is_object()) {
        return false;
    }

    if (value.contains("ipv4")) {
        if (!value.at("ipv4").is_number_unsigned()) {
            return false;
        }
        message.set_ipv4(value.at("ipv4").get<uint32_t>());
    }

    if (value.contains("ipv6")) {
        std::string bytes;
        if (!decodeBytes(value.at("ipv6"), bytes)) {
            return false;
        }
        message.set_ipv6(bytes);
    }

    return true;
}

json toJson(const rsp::proto::PortRange& message) {
    return json{{"start_port", message.start_port()}, {"end_port", message.end_port()}};
}

bool fromJson(const json& value, rsp::proto::PortRange& message) {
    if (!value.is_object() || !value.contains("start_port") || !value.contains("end_port") ||
        !value.at("start_port").is_number_unsigned() || !value.at("end_port").is_number_unsigned()) {
        return false;
    }

    message.set_start_port(value.at("start_port").get<uint32_t>());
    message.set_end_port(value.at("end_port").get<uint32_t>());
    return true;
}

json toJson(const rsp::proto::ResourceTCPConnect& message) {
    json addresses = json::array();
    for (const auto& address : message.source_addresses()) {
        addresses.push_back(toJson(address));
    }

    json value{{"source_addresses", std::move(addresses)}};
    if (message.has_node_id()) {
        value["node_id"] = toJson(message.node_id());
    }
    return value;
}

bool fromJson(const json& value, rsp::proto::ResourceTCPConnect& message) {
    if (!value.is_object() || !value.contains("source_addresses") || !value.at("source_addresses").is_array()) {
        return false;
    }

    if (value.contains("node_id") && !fromJson(value.at("node_id"), *message.mutable_node_id())) {
        return false;
    }

    for (const auto& address : value.at("source_addresses")) {
        if (!fromJson(address, *message.add_source_addresses())) {
            return false;
        }
    }
    return true;
}

json toJson(const rsp::proto::ResourceTCPListen& message) {
    json addresses = json::array();
    for (const auto& address : message.listen_address()) {
        addresses.push_back(toJson(address));
    }

    json value{{"listen_address", std::move(addresses)},
               {"allowed_range", toJson(message.allowed_range())}};
    if (message.has_node_id()) {
        value["node_id"] = toJson(message.node_id());
    }
    return value;
}

bool fromJson(const json& value, rsp::proto::ResourceTCPListen& message) {
    if (!value.is_object() || !value.contains("listen_address") || !value.at("listen_address").is_array() ||
        !value.contains("allowed_range")) {
        return false;
    }

    if (value.contains("node_id") && !fromJson(value.at("node_id"), *message.mutable_node_id())) {
        return false;
    }

    for (const auto& address : value.at("listen_address")) {
        if (!fromJson(address, *message.add_listen_address())) {
            return false;
        }
    }

    return fromJson(value.at("allowed_range"), *message.mutable_allowed_range());
}

json toJson(const rsp::proto::ResourceRecord& message) {
    json value = json::object();
    if (message.has_tcp_connect()) {
        value["tcp_connect"] = toJson(message.tcp_connect());
    } else if (message.has_tcp_listen()) {
        value["tcp_listen"] = toJson(message.tcp_listen());
    }
    return value;
}

bool fromJson(const json& value, rsp::proto::ResourceRecord& message) {
    if (!value.is_object()) {
        return false;
    }

    if (value.contains("tcp_connect")) {
        return fromJson(value.at("tcp_connect"), *message.mutable_tcp_connect());
    }

    if (value.contains("tcp_listen")) {
        return fromJson(value.at("tcp_listen"), *message.mutable_tcp_listen());
    }

    return false;
}

json toJson(const rsp::proto::ResourceAdvertisement& message) {
    json records = json::array();
    for (const auto& record : message.records()) {
        records.push_back(toJson(record));
    }
    return json{{"records", std::move(records)}};
}

bool fromJson(const json& value, rsp::proto::ResourceAdvertisement& message) {
    if (!value.is_object() || !value.contains("records") || !value.at("records").is_array()) {
        return false;
    }

    for (const auto& record : value.at("records")) {
        if (!fromJson(record, *message.add_records())) {
            return false;
        }
    }
    return true;
}

json toJson(const rsp::proto::ResourceQuery& message) {
    return json{{"query", message.query()}, {"max_records", message.max_records()}};
}

bool fromJson(const json& value, rsp::proto::ResourceQuery& message) {
    if (!value.is_object() || !value.contains("query") || !value.contains("max_records") ||
        !value.at("query").is_string() || !value.at("max_records").is_number_unsigned()) {
        return false;
    }
    message.set_query(value.at("query").get<std::string>());
    message.set_max_records(value.at("max_records").get<uint32_t>());
    return true;
}

json toJson(const rsp::proto::Error& message) {
    return json{{"error_code", encodeEnum(message.error_code())}, {"message", message.message()}};
}

bool fromJson(const json& value, rsp::proto::Error& message) {
    if (!value.is_object() || !value.contains("error_code") || !value.contains("message") ||
        !value.at("message").is_string()) {
        return false;
    }

    rsp::proto::ERROR_CODE errorCode = rsp::proto::UNKNOWN_ERROR;
    if (!decodeEnum(value.at("error_code"), errorCode)) {
        return false;
    }

    message.set_error_code(errorCode);
    message.set_message(value.at("message").get<std::string>());
    return true;
}

json toJson(const rsp::proto::PingRequest& message) {
    return json{{"nonce", toJson(message.nonce())},
                {"sequence", message.sequence()},
                {"time_sent", toJson(message.time_sent())}};
}

bool fromJson(const json& value, rsp::proto::PingRequest& message) {
    if (!value.is_object() || !value.contains("nonce") || !value.contains("sequence") || !value.contains("time_sent") ||
        !value.at("sequence").is_number_unsigned()) {
        return false;
    }

    return fromJson(value.at("nonce"), *message.mutable_nonce()) &&
           fromJson(value.at("time_sent"), *message.mutable_time_sent()) &&
           (message.set_sequence(value.at("sequence").get<uint32_t>()), true);
}

json toJson(const rsp::proto::PingReply& message) {
    return json{{"nonce", toJson(message.nonce())},
                {"sequence", message.sequence()},
                {"time_sent", toJson(message.time_sent())},
                {"time_replied", toJson(message.time_replied())}};
}

bool fromJson(const json& value, rsp::proto::PingReply& message) {
    if (!value.is_object() || !value.contains("nonce") || !value.contains("sequence") || !value.contains("time_sent") ||
        !value.contains("time_replied") || !value.at("sequence").is_number_unsigned()) {
        return false;
    }

    return fromJson(value.at("nonce"), *message.mutable_nonce()) &&
           fromJson(value.at("time_sent"), *message.mutable_time_sent()) &&
           fromJson(value.at("time_replied"), *message.mutable_time_replied()) &&
           (message.set_sequence(value.at("sequence").get<uint32_t>()), true);
}

json toJson(const rsp::proto::SocketReply& message) {
    json value{{"socket_id", toJson(message.socket_id())}, {"error", encodeEnum(message.error())}};
    if (message.has_message()) {
        value["message"] = message.message();
    }
    if (message.has_new_socket_remote_address()) {
        value["new_socket_remote_address"] = message.new_socket_remote_address();
    }
    if (message.has_new_socket_id()) {
        value["new_socket_id"] = toJson(message.new_socket_id());
    }
    if (message.has_socket_error_code()) {
        value["socket_error_code"] = message.socket_error_code();
    }
    if (message.has_data()) {
        value["data"] = encodeBytes(message.data());
    }
    return value;
}

bool fromJson(const json& value, rsp::proto::SocketReply& message) {
    if (!value.is_object() || !value.contains("socket_id") || !value.contains("error")) {
        return false;
    }

    rsp::proto::SOCKET_STATUS status = rsp::proto::SUCCESS;
    if (!fromJson(value.at("socket_id"), *message.mutable_socket_id()) || !decodeEnum(value.at("error"), status)) {
        return false;
    }
    message.set_error(status);

    if (value.contains("message")) {
        if (!value.at("message").is_string()) {
            return false;
        }
        message.set_message(value.at("message").get<std::string>());
    }
    if (value.contains("new_socket_remote_address")) {
        if (!value.at("new_socket_remote_address").is_string()) {
            return false;
        }
        message.set_new_socket_remote_address(value.at("new_socket_remote_address").get<std::string>());
    }
    if (value.contains("new_socket_id") && !fromJson(value.at("new_socket_id"), *message.mutable_new_socket_id())) {
        return false;
    }
    if (value.contains("socket_error_code")) {
        if (!value.at("socket_error_code").is_number_integer()) {
            return false;
        }
        message.set_socket_error_code(value.at("socket_error_code").get<int32_t>());
    }
    if (value.contains("data")) {
        std::string data;
        if (!decodeBytes(value.at("data"), data)) {
            return false;
        }
        message.set_data(data);
    }
    return true;
}

json toJson(const rsp::proto::ConnectTCPRequest& message) {
    json value{{"host_port", message.host_port()}, {"socket_number", toJson(message.socket_number())}};
    if (message.has_reuse_addr()) {
        value["reuse_addr"] = message.reuse_addr();
    }
    if (message.has_source_port()) {
        value["source_port"] = message.source_port();
    }
    if (message.has_timeout_ms()) {
        value["timeout_ms"] = message.timeout_ms();
    }
    if (message.has_retries()) {
        value["retries"] = message.retries();
    }
    if (message.has_retry_ms()) {
        value["retry_ms"] = message.retry_ms();
    }
    if (message.has_async_data()) {
        value["async_data"] = message.async_data();
    }
    if (message.has_use_socket()) {
        value["use_socket"] = message.use_socket();
    }
    if (message.has_share_socket()) {
        value["share_socket"] = message.share_socket();
    }
    return value;
}

bool fromJson(const json& value, rsp::proto::ConnectTCPRequest& message) {
    if (!value.is_object() || !value.contains("host_port") || !value.contains("socket_number") ||
        !value.at("host_port").is_string()) {
        return false;
    }

    message.set_host_port(value.at("host_port").get<std::string>());
    if (!fromJson(value.at("socket_number"), *message.mutable_socket_number())) {
        return false;
    }

    auto decodeBool = [&](const char* key, auto setter) {
        if (!value.contains(key)) {
            return true;
        }
        if (!value.at(key).is_boolean()) {
            return false;
        }
        setter(value.at(key).get<bool>());
        return true;
    };
    auto decodeUnsigned = [&](const char* key, auto setter) {
        if (!value.contains(key)) {
            return true;
        }
        if (!value.at(key).is_number_unsigned()) {
            return false;
        }
        setter(value.at(key).get<uint32_t>());
        return true;
    };

    return decodeBool("reuse_addr", [&](bool v) { message.set_reuse_addr(v); }) &&
           decodeUnsigned("source_port", [&](uint32_t v) { message.set_source_port(v); }) &&
           decodeUnsigned("timeout_ms", [&](uint32_t v) { message.set_timeout_ms(v); }) &&
           decodeUnsigned("retries", [&](uint32_t v) { message.set_retries(v); }) &&
           decodeUnsigned("retry_ms", [&](uint32_t v) { message.set_retry_ms(v); }) &&
           decodeBool("async_data", [&](bool v) { message.set_async_data(v); }) &&
           decodeBool("use_socket", [&](bool v) { message.set_use_socket(v); }) &&
           decodeBool("share_socket", [&](bool v) { message.set_share_socket(v); });
}

json toJson(const rsp::proto::ListenTCPRequest& message) {
    json value{{"host_port", message.host_port()}, {"socket_number", toJson(message.socket_number())}};
    if (message.has_reuse_addr()) value["reuse_addr"] = message.reuse_addr();
    if (message.has_timeout_ms()) value["timeout_ms"] = message.timeout_ms();
    if (message.has_async_accept()) value["async_accept"] = message.async_accept();
    if (message.has_share_listening_socket()) value["share_listening_socket"] = message.share_listening_socket();
    if (message.has_share_child_sockets()) value["share_child_sockets"] = message.share_child_sockets();
    if (message.has_children_use_socket()) value["children_use_socket"] = message.children_use_socket();
    if (message.has_children_async_data()) value["children_async_data"] = message.children_async_data();
    return value;
}

bool fromJson(const json& value, rsp::proto::ListenTCPRequest& message) {
    if (!value.is_object() || !value.contains("host_port") || !value.contains("socket_number") ||
        !value.at("host_port").is_string() || !fromJson(value.at("socket_number"), *message.mutable_socket_number())) {
        return false;
    }
    message.set_host_port(value.at("host_port").get<std::string>());

    auto decodeBool = [&](const char* key, auto setter) {
        if (!value.contains(key)) return true;
        if (!value.at(key).is_boolean()) return false;
        setter(value.at(key).get<bool>());
        return true;
    };
    auto decodeUnsigned = [&](const char* key, auto setter) {
        if (!value.contains(key)) return true;
        if (!value.at(key).is_number_unsigned()) return false;
        setter(value.at(key).get<uint32_t>());
        return true;
    };

    return decodeBool("reuse_addr", [&](bool v) { message.set_reuse_addr(v); }) &&
           decodeUnsigned("timeout_ms", [&](uint32_t v) { message.set_timeout_ms(v); }) &&
           decodeBool("async_accept", [&](bool v) { message.set_async_accept(v); }) &&
           decodeBool("share_listening_socket", [&](bool v) { message.set_share_listening_socket(v); }) &&
           decodeBool("share_child_sockets", [&](bool v) { message.set_share_child_sockets(v); }) &&
           decodeBool("children_use_socket", [&](bool v) { message.set_children_use_socket(v); }) &&
           decodeBool("children_async_data", [&](bool v) { message.set_children_async_data(v); });
}

json toJson(const rsp::proto::AcceptTCP& message) {
    json value{{"listen_socket_number", toJson(message.listen_socket_number())}};
    if (message.has_new_socket_number()) value["new_socket_number"] = toJson(message.new_socket_number());
    if (message.has_timeout_ms()) value["timeout_ms"] = message.timeout_ms();
    if (message.has_share_child_socket()) value["share_child_socket"] = message.share_child_socket();
    if (message.has_child_use_socket()) value["child_use_socket"] = message.child_use_socket();
    if (message.has_child_async_data()) value["child_async_data"] = message.child_async_data();
    return value;
}

bool fromJson(const json& value, rsp::proto::AcceptTCP& message) {
    if (!value.is_object() || !value.contains("listen_socket_number") ||
        !fromJson(value.at("listen_socket_number"), *message.mutable_listen_socket_number())) {
        return false;
    }
    if (value.contains("new_socket_number") &&
        !fromJson(value.at("new_socket_number"), *message.mutable_new_socket_number())) {
        return false;
    }
    if (value.contains("timeout_ms")) {
        if (!value.at("timeout_ms").is_number_unsigned()) return false;
        message.set_timeout_ms(value.at("timeout_ms").get<uint32_t>());
    }
    if (value.contains("share_child_socket")) {
        if (!value.at("share_child_socket").is_boolean()) return false;
        message.set_share_child_socket(value.at("share_child_socket").get<bool>());
    }
    if (value.contains("child_use_socket")) {
        if (!value.at("child_use_socket").is_boolean()) return false;
        message.set_child_use_socket(value.at("child_use_socket").get<bool>());
    }
    if (value.contains("child_async_data")) {
        if (!value.at("child_async_data").is_boolean()) return false;
        message.set_child_async_data(value.at("child_async_data").get<bool>());
    }
    return true;
}

json toJson(const rsp::proto::SocketSend& message) {
    return json{{"socket_number", toJson(message.socket_number())},
                {"index", message.index()},
                {"data", encodeBytes(message.data())}};
}

bool fromJson(const json& value, rsp::proto::SocketSend& message) {
    if (!value.is_object() || !value.contains("socket_number") || !value.contains("index") || !value.contains("data") ||
        !value.at("index").is_number_unsigned()) {
        return false;
    }

    std::string data;
    return fromJson(value.at("socket_number"), *message.mutable_socket_number()) &&
           decodeBytes(value.at("data"), data) &&
           (message.set_index(value.at("index").get<uint64_t>()), message.set_data(data), true);
}

json toJson(const rsp::proto::SocketRecv& message) {
    json value{{"socket_number", toJson(message.socket_number())}, {"index", message.index()}};
    if (message.has_max_bytes()) value["max_bytes"] = message.max_bytes();
    if (message.has_wait_ms()) value["wait_ms"] = message.wait_ms();
    return value;
}

bool fromJson(const json& value, rsp::proto::SocketRecv& message) {
    if (!value.is_object() || !value.contains("socket_number") || !value.contains("index") ||
        !value.at("index").is_number_unsigned() ||
        !fromJson(value.at("socket_number"), *message.mutable_socket_number())) {
        return false;
    }
    message.set_index(value.at("index").get<uint64_t>());
    if (value.contains("max_bytes")) {
        if (!value.at("max_bytes").is_number_unsigned()) return false;
        message.set_max_bytes(value.at("max_bytes").get<uint32_t>());
    }
    if (value.contains("wait_ms")) {
        if (!value.at("wait_ms").is_number_unsigned()) return false;
        message.set_wait_ms(value.at("wait_ms").get<uint32_t>());
    }
    return true;
}

json toJson(const rsp::proto::SocketClose& message) {
    return json{{"socket_number", toJson(message.socket_number())}};
}

bool fromJson(const json& value, rsp::proto::SocketClose& message) {
    return value.is_object() && value.contains("socket_number") &&
           fromJson(value.at("socket_number"), *message.mutable_socket_number());
}

json toJson(const rsp::proto::BeginEndorsementRequest& message) {
    json value{{"requested_values", toJson(message.requested_values())}};
    if (message.has_auth_data()) {
        value["auth_data"] = encodeBytes(message.auth_data());
    }
    return value;
}

bool fromJson(const json& value, rsp::proto::BeginEndorsementRequest& message) {
    if (!value.is_object() || !value.contains("requested_values") ||
        !fromJson(value.at("requested_values"), *message.mutable_requested_values())) {
        return false;
    }
    if (value.contains("auth_data")) {
        std::string authData;
        if (!decodeBytes(value.at("auth_data"), authData)) {
            return false;
        }
        message.set_auth_data(authData);
    }
    return true;
}

json toJson(const rsp::proto::EndorsementChallenge& message) {
    json value = json::object();
    if (message.has_stage()) value["stage"] = message.stage();
    if (message.has_challenge()) value["challenge"] = encodeBytes(message.challenge());
    return value;
}

bool fromJson(const json& value, rsp::proto::EndorsementChallenge& message) {
    if (!value.is_object()) return false;
    if (value.contains("stage")) {
        if (!value.at("stage").is_number_unsigned()) return false;
        message.set_stage(value.at("stage").get<uint32_t>());
    }
    if (value.contains("challenge")) {
        std::string challenge;
        if (!decodeBytes(value.at("challenge"), challenge)) return false;
        message.set_challenge(challenge);
    }
    return true;
}

json toJson(const rsp::proto::EndorsementChallengeReply& message) {
    json value = json::object();
    if (message.has_stage()) value["stage"] = message.stage();
    if (message.has_challenge_reply()) value["challenge_reply"] = encodeBytes(message.challenge_reply());
    return value;
}

bool fromJson(const json& value, rsp::proto::EndorsementChallengeReply& message) {
    if (!value.is_object()) return false;
    if (value.contains("stage")) {
        if (!value.at("stage").is_number_unsigned()) return false;
        message.set_stage(value.at("stage").get<uint32_t>());
    }
    if (value.contains("challenge_reply")) {
        std::string reply;
        if (!decodeBytes(value.at("challenge_reply"), reply)) return false;
        message.set_challenge_reply(reply);
    }
    return true;
}

json toJson(const rsp::proto::EndorsementDone& message) {
    return json{{"status", encodeEnum(message.status())}, {"new_endorsement", toJson(message.new_endorsement())}};
}

bool fromJson(const json& value, rsp::proto::EndorsementDone& message) {
    if (!value.is_object() || !value.contains("status") || !value.contains("new_endorsement")) {
        return false;
    }
    rsp::proto::ENSDORSMENT_STATUS status = rsp::proto::ENDORSEMENT_SUCCESS;
    return decodeEnum(value.at("status"), status) &&
           fromJson(value.at("new_endorsement"), *message.mutable_new_endorsement()) &&
           (message.set_status(status), true);
}

json toJson(const rsp::proto::ERDAbstractSyntaxTree& message);
bool fromJson(const json& value, rsp::proto::ERDAbstractSyntaxTree& message);

json toJson(const rsp::proto::ERDASTEquals& message) {
    return json{{"lhs", toJson(message.lhs())}, {"rhs", toJson(message.rhs())}};
}

bool fromJson(const json& value, rsp::proto::ERDASTEquals& message) {
    return value.is_object() && value.contains("lhs") && value.contains("rhs") &&
           fromJson(value.at("lhs"), *message.mutable_lhs()) &&
           fromJson(value.at("rhs"), *message.mutable_rhs());
}

json toJson(const rsp::proto::ERDASTAnd& message) {
    return json{{"lhs", toJson(message.lhs())}, {"rhs", toJson(message.rhs())}};
}

bool fromJson(const json& value, rsp::proto::ERDASTAnd& message) {
    return value.is_object() && value.contains("lhs") && value.contains("rhs") &&
           fromJson(value.at("lhs"), *message.mutable_lhs()) &&
           fromJson(value.at("rhs"), *message.mutable_rhs());
}

json toJson(const rsp::proto::ERDASTOr& message) {
    return json{{"lhs", toJson(message.lhs())}, {"rhs", toJson(message.rhs())}};
}

bool fromJson(const json& value, rsp::proto::ERDASTOr& message) {
    return value.is_object() && value.contains("lhs") && value.contains("rhs") &&
           fromJson(value.at("lhs"), *message.mutable_lhs()) &&
           fromJson(value.at("rhs"), *message.mutable_rhs());
}

json toJson(const rsp::proto::ERDASTAllOf& message) {
    json terms = json::array();
    for (const auto& term : message.terms()) {
        terms.push_back(toJson(term));
    }
    return json{{"terms", std::move(terms)}};
}

bool fromJson(const json& value, rsp::proto::ERDASTAllOf& message) {
    if (!value.is_object() || !value.contains("terms") || !value.at("terms").is_array()) return false;
    for (const auto& term : value.at("terms")) {
        if (!fromJson(term, *message.add_terms())) return false;
    }
    return true;
}

json toJson(const rsp::proto::ERDASTAnyOf& message) {
    json terms = json::array();
    for (const auto& term : message.terms()) {
        terms.push_back(toJson(term));
    }
    return json{{"terms", std::move(terms)}};
}

bool fromJson(const json& value, rsp::proto::ERDASTAnyOf& message) {
    if (!value.is_object() || !value.contains("terms") || !value.at("terms").is_array()) return false;
    for (const auto& term : value.at("terms")) {
        if (!fromJson(term, *message.add_terms())) return false;
    }
    return true;
}

json toJson(const rsp::proto::ERDASTEndorsementTypeEquals& message) {
    return json{{"type", toJson(message.type())}};
}

bool fromJson(const json& value, rsp::proto::ERDASTEndorsementTypeEquals& message) {
    return value.is_object() && value.contains("type") && fromJson(value.at("type"), *message.mutable_type());
}

json toJson(const rsp::proto::ERDASTEndorsementValueEquals& message) {
    return json{{"value", encodeBytes(message.value())}};
}

bool fromJson(const json& value, rsp::proto::ERDASTEndorsementValueEquals& message) {
    if (!value.is_object() || !value.contains("value")) return false;
    std::string bytes;
    if (!decodeBytes(value.at("value"), bytes)) return false;
    message.set_value(bytes);
    return true;
}

json toJson(const rsp::proto::ERDASTEndorsementSignerEquals& message) {
    return json{{"signer", toJson(message.signer())}};
}

bool fromJson(const json& value, rsp::proto::ERDASTEndorsementSignerEquals& message) {
    return value.is_object() && value.contains("signer") && fromJson(value.at("signer"), *message.mutable_signer());
}

json toJson(const rsp::proto::ERDASTMessageDestination& message) {
    return json{{"destination", toJson(message.destination())}};
}

bool fromJson(const json& value, rsp::proto::ERDASTMessageDestination& message) {
    return value.is_object() && value.contains("destination") &&
           fromJson(value.at("destination"), *message.mutable_destination());
}

json toJson(const rsp::proto::ERDASTMessageSource& message) {
    return json{{"source", toJson(message.source())}};
}

bool fromJson(const json& value, rsp::proto::ERDASTMessageSource& message) {
    return value.is_object() && value.contains("source") && fromJson(value.at("source"), *message.mutable_source());
}

json toJson(const rsp::proto::ERDAbstractSyntaxTree& message) {
    json value = json::object();
    if (message.has_equals()) value["equals"] = toJson(message.equals());
    else if (message.has_and_()) value["and"] = toJson(message.and_());
    else if (message.has_or_()) value["or"] = toJson(message.or_());
    else if (message.has_endorsement_type_equals()) value["endorsement_type_equals"] = toJson(message.endorsement_type_equals());
    else if (message.has_endorsement_value_equals()) value["endorsement_value_equals"] = toJson(message.endorsement_value_equals());
    else if (message.has_endorsement_signer_equals()) value["endorsement_signer_equals"] = toJson(message.endorsement_signer_equals());
    else if (message.has_message_destination()) value["message_destination"] = toJson(message.message_destination());
    else if (message.has_message_source()) value["message_source"] = toJson(message.message_source());
    else if (message.has_true_value()) value["true_value"] = json::object();
    else if (message.has_false_value()) value["false_value"] = json::object();
    else if (message.has_all_of()) value["all_of"] = toJson(message.all_of());
    else if (message.has_any_of()) value["any_of"] = toJson(message.any_of());
    return value;
}

bool fromJson(const json& value, rsp::proto::ERDAbstractSyntaxTree& message) {
    if (!value.is_object()) return false;
    if (value.contains("equals")) return fromJson(value.at("equals"), *message.mutable_equals());
    if (value.contains("and")) return fromJson(value.at("and"), *message.mutable_and_());
    if (value.contains("or")) return fromJson(value.at("or"), *message.mutable_or_());
    if (value.contains("endorsement_type_equals")) return fromJson(value.at("endorsement_type_equals"), *message.mutable_endorsement_type_equals());
    if (value.contains("endorsement_value_equals")) return fromJson(value.at("endorsement_value_equals"), *message.mutable_endorsement_value_equals());
    if (value.contains("endorsement_signer_equals")) return fromJson(value.at("endorsement_signer_equals"), *message.mutable_endorsement_signer_equals());
    if (value.contains("message_destination")) return fromJson(value.at("message_destination"), *message.mutable_message_destination());
    if (value.contains("message_source")) return fromJson(value.at("message_source"), *message.mutable_message_source());
    if (value.contains("true_value")) {
        message.mutable_true_value();
        return true;
    }
    if (value.contains("false_value")) {
        message.mutable_false_value();
        return true;
    }
    if (value.contains("all_of")) return fromJson(value.at("all_of"), *message.mutable_all_of());
    if (value.contains("any_of")) return fromJson(value.at("any_of"), *message.mutable_any_of());
    return false;
}

json toJson(const rsp::proto::EndorsementNeeded& message) {
    return json{{"message_nonce", toJson(message.message_nonce())}, {"tree", toJson(message.tree())}};
}

bool fromJson(const json& value, rsp::proto::EndorsementNeeded& message) {
    return value.is_object() && value.contains("message_nonce") && value.contains("tree") &&
           fromJson(value.at("message_nonce"), *message.mutable_message_nonce()) &&
           fromJson(value.at("tree"), *message.mutable_tree());
}

json toJson(const rsp::proto::RSPMessage& message) {
    json value = json::object();
    if (message.has_destination()) value["destination"] = toJson(message.destination());
    if (message.has_source()) value["source"] = toJson(message.source());

    if (message.has_challenge_request()) value["challenge_request"] = toJson(message.challenge_request());
    else if (message.has_identity()) value["identity"] = toJson(message.identity());
    else if (message.has_route()) value["route"] = toJson(message.route());
    else if (message.has_error()) value["error"] = toJson(message.error());
    else if (message.has_ping_request()) value["ping_request"] = toJson(message.ping_request());
    else if (message.has_ping_reply()) value["ping_reply"] = toJson(message.ping_reply());
    else if (message.has_connect_tcp_request()) value["connect_tcp_request"] = toJson(message.connect_tcp_request());
    else if (message.has_socket_reply()) value["socket_reply"] = toJson(message.socket_reply());
    else if (message.has_socket_send()) value["socket_send"] = toJson(message.socket_send());
    else if (message.has_socket_recv()) value["socket_recv"] = toJson(message.socket_recv());
    else if (message.has_socket_close()) value["socket_close"] = toJson(message.socket_close());
    else if (message.has_listen_tcp_request()) value["listen_tcp_request"] = toJson(message.listen_tcp_request());
    else if (message.has_accept_tcp()) value["accept_tcp"] = toJson(message.accept_tcp());
    else if (message.has_resource_advertisement()) value["resource_advertisement"] = toJson(message.resource_advertisement());
    else if (message.has_resource_query()) value["resource_query"] = toJson(message.resource_query());
    else if (message.has_begin_endorsement_request()) value["begin_endorsement_request"] = toJson(message.begin_endorsement_request());
    else if (message.has_endorsement_challenge()) value["endorsement_challenge"] = toJson(message.endorsement_challenge());
    else if (message.has_endorsement_challenge_reply()) value["endorsement_challenge_reply"] = toJson(message.endorsement_challenge_reply());
    else if (message.has_endorsement_done()) value["endorsement_done"] = toJson(message.endorsement_done());
    else if (message.has_endorsement_needed()) value["endorsement_needed"] = toJson(message.endorsement_needed());

    if (message.has_nonce()) value["nonce"] = toJson(message.nonce());
    if (message.has_signature()) value["signature"] = toJson(message.signature());

    json endorsements = json::array();
    for (const auto& endorsement : message.endorsements()) {
        endorsements.push_back(toJson(endorsement));
    }
    if (!endorsements.empty()) {
        value["endorsements"] = std::move(endorsements);
    }

    return value;
}

bool fromJson(const json& value, rsp::proto::RSPMessage& message) {
    if (!value.is_object()) {
        return false;
    }

    if (value.contains("destination") && !fromJson(value.at("destination"), *message.mutable_destination())) return false;
    if (value.contains("source") && !fromJson(value.at("source"), *message.mutable_source())) return false;
    if (value.contains("challenge_request") && !fromJson(value.at("challenge_request"), *message.mutable_challenge_request())) return false;
    if (value.contains("identity") && !fromJson(value.at("identity"), *message.mutable_identity())) return false;
    if (value.contains("route") && !fromJson(value.at("route"), *message.mutable_route())) return false;
    if (value.contains("error") && !fromJson(value.at("error"), *message.mutable_error())) return false;
    if (value.contains("ping_request") && !fromJson(value.at("ping_request"), *message.mutable_ping_request())) return false;
    if (value.contains("ping_reply") && !fromJson(value.at("ping_reply"), *message.mutable_ping_reply())) return false;
    if (value.contains("connect_tcp_request") && !fromJson(value.at("connect_tcp_request"), *message.mutable_connect_tcp_request())) return false;
    if (value.contains("socket_reply") && !fromJson(value.at("socket_reply"), *message.mutable_socket_reply())) return false;
    if (value.contains("socket_send") && !fromJson(value.at("socket_send"), *message.mutable_socket_send())) return false;
    if (value.contains("socket_recv") && !fromJson(value.at("socket_recv"), *message.mutable_socket_recv())) return false;
    if (value.contains("socket_close") && !fromJson(value.at("socket_close"), *message.mutable_socket_close())) return false;
    if (value.contains("listen_tcp_request") && !fromJson(value.at("listen_tcp_request"), *message.mutable_listen_tcp_request())) return false;
    if (value.contains("accept_tcp") && !fromJson(value.at("accept_tcp"), *message.mutable_accept_tcp())) return false;
    if (value.contains("resource_advertisement") && !fromJson(value.at("resource_advertisement"), *message.mutable_resource_advertisement())) return false;
    if (value.contains("resource_query") && !fromJson(value.at("resource_query"), *message.mutable_resource_query())) return false;
    if (value.contains("begin_endorsement_request") && !fromJson(value.at("begin_endorsement_request"), *message.mutable_begin_endorsement_request())) return false;
    if (value.contains("endorsement_challenge") && !fromJson(value.at("endorsement_challenge"), *message.mutable_endorsement_challenge())) return false;
    if (value.contains("endorsement_challenge_reply") && !fromJson(value.at("endorsement_challenge_reply"), *message.mutable_endorsement_challenge_reply())) return false;
    if (value.contains("endorsement_done") && !fromJson(value.at("endorsement_done"), *message.mutable_endorsement_done())) return false;
    if (value.contains("endorsement_needed") && !fromJson(value.at("endorsement_needed"), *message.mutable_endorsement_needed())) return false;
    if (value.contains("nonce") && !fromJson(value.at("nonce"), *message.mutable_nonce())) return false;
    if (value.contains("signature") && !fromJson(value.at("signature"), *message.mutable_signature())) return false;
    if (value.contains("endorsements")) {
        if (!value.at("endorsements").is_array()) return false;
        for (const auto& endorsement : value.at("endorsements")) {
            if (!fromJson(endorsement, *message.add_endorsements())) return false;
        }
    }

    return true;
}

std::string encodeMessage(const rsp::proto::RSPMessage& message) {
    return toJson(message).dump();
}

bool decodeMessage(const std::string& payload, rsp::proto::RSPMessage& message) {
    json parsed = json::parse(payload, nullptr, false);
    return !parsed.is_discarded() && fromJson(parsed, message);
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

    return decodeMessage(payload, message);
}

bool JsonEncoding::writeMessage(const rsp::proto::RSPMessage& message) {
    const rsp::transport::ConnectionHandle activeConnection = connection();
    if (activeConnection == nullptr) {
        return false;
    }

    const std::string payload = encodeMessage(message);
    if (payload.size() > kMaxFrameLength) {
        return false;
    }

    std::string header;
    header.reserve(kFrameHeaderSize);
    appendUint32(header, kFrameMagic);
    appendUint32(header, static_cast<uint32_t>(payload.size()));

    return activeConnection->sendAll(reinterpret_cast<const uint8_t*>(header.data()), static_cast<uint32_t>(header.size())) &&
           activeConnection->sendAll(reinterpret_cast<const uint8_t*>(payload.data()), static_cast<uint32_t>(payload.size()));
}

}  // namespace rsp::encoding::json