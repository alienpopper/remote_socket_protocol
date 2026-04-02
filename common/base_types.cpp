#include "common/base_types.hpp"

#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace rsp {

namespace {

bool isHexCharacter(char value) {
    return (value >= '0' && value <= '9') ||
           (value >= 'a' && value <= 'f') ||
           (value >= 'A' && value <= 'F');
}

}  // namespace

GUID::GUID() : high_(0), low_(0) {
}

GUID::GUID(uint64_t high, uint64_t low) : high_(high), low_(low) {
}

GUID::GUID(const std::string& guidString) : GUID(parse(guidString)) {
}

GUID::GUID(const char* guidString) : GUID(std::string(guidString == nullptr ? "" : guidString)) {
}

std::string GUID::toString() const {
    std::ostringstream stream;
    stream << std::hex << std::setfill('0') << std::nouppercase
           << std::setw(8) << static_cast<uint32_t>(high_ >> 32) << '-'
           << std::setw(4) << static_cast<uint16_t>((high_ >> 16) & 0xffffU) << '-'
           << std::setw(4) << static_cast<uint16_t>(high_ & 0xffffU) << '-'
           << std::setw(4) << static_cast<uint16_t>(low_ >> 48) << '-'
           << std::setw(12) << (low_ & 0x0000FFFFFFFFFFFFULL);
    return stream.str();
}

GUID::operator std::string() const {
    return toString();
}

uint64_t GUID::high() const {
    return high_;
}

uint64_t GUID::low() const {
    return low_;
}

bool GUID::operator==(const GUID& other) const {
    return high_ == other.high_ && low_ == other.low_;
}

bool GUID::operator!=(const GUID& other) const {
    return !(*this == other);
}

GUID GUID::parse(const std::string& guidString) {
    const std::string normalized = normalize(guidString);
    return GUID(parseHex64(normalized.substr(0, 16)), parseHex64(normalized.substr(16, 16)));
}

NodeID::NodeID() = default;

NodeID::NodeID(uint64_t high, uint64_t low) : GUID(high, low) {
}

NodeID::NodeID(const GUID& guid) : GUID(guid.high(), guid.low()) {
}

NodeID::NodeID(const std::string& guidString) : GUID(guidString) {
}

NodeID::NodeID(const char* guidString) : GUID(guidString) {
}

uint64_t GUID::parseHex64(const std::string& text) {
    uint64_t value = 0;
    for (const char current : text) {
        value <<= 4;

        if (current >= '0' && current <= '9') {
            value |= static_cast<uint64_t>(current - '0');
        } else if (current >= 'a' && current <= 'f') {
            value |= static_cast<uint64_t>(current - 'a' + 10);
        } else if (current >= 'A' && current <= 'F') {
            value |= static_cast<uint64_t>(current - 'A' + 10);
        } else {
            throw std::invalid_argument("GUID contains a non-hex character");
        }
    }

    return value;
}

std::string GUID::normalize(const std::string& guidString) {
    std::string compact;
    compact.reserve(32);

    for (const char current : guidString) {
        if (current == '{' || current == '}' || current == '-') {
            continue;
        }

        if (!isHexCharacter(current)) {
            throw std::invalid_argument("GUID contains an invalid character");
        }

        compact.push_back(current);
    }

    if (compact.size() != 32) {
        throw std::invalid_argument("GUID must contain exactly 32 hex digits");
    }

    return compact;
}

}  // namespace rsp