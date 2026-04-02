#include "common/base_types.hpp"

#include "os/os_random.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
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

Buffer::Buffer() : size_(0) {
}

Buffer::Buffer(uint32_t size) : bytes_(size == 0 ? nullptr : std::make_unique<uint8_t[]>(size)), size_(size) {
}

Buffer::Buffer(const uint8_t* data, uint32_t size) : Buffer(size) {
    if (data != nullptr && size_ != 0) {
        std::copy_n(data, size_, bytes_.get());
    }
}

Buffer::Buffer(const Buffer& other) : Buffer(other.data(), other.size()) {
}

Buffer& Buffer::operator=(const Buffer& other) {
    if (this != &other) {
        Buffer copy(other);
        *this = std::move(copy);
    }

    return *this;
}

Buffer::Buffer(Buffer&& other) noexcept = default;

Buffer& Buffer::operator=(Buffer&& other) noexcept = default;

uint8_t* Buffer::data() {
    return bytes_.get();
}

const uint8_t* Buffer::data() const {
    return bytes_.get();
}

uint32_t Buffer::size() const {
    return size_;
}

bool Buffer::empty() const {
    return size_ == 0;
}

void Buffer::resize(uint32_t size) {
    if (size == size_) {
        return;
    }

    std::unique_ptr<uint8_t[]> resized = size == 0 ? nullptr : std::make_unique<uint8_t[]>(size);
    const uint32_t bytesToCopy = size < size_ ? size : size_;
    if (bytesToCopy != 0 && bytes_ != nullptr) {
        std::copy_n(bytes_.get(), bytesToCopy, resized.get());
    }

    bytes_ = std::move(resized);
    size_ = size;
}

GUID::GUID() : high_(0), low_(0) {
    std::array<uint8_t, 16> randomBytes{};
    rsp::os::randomFill(randomBytes.data(), static_cast<uint32_t>(randomBytes.size()));

    std::memcpy(&high_, randomBytes.data(), sizeof(high_));
    std::memcpy(&low_, randomBytes.data() + sizeof(high_), sizeof(low_));
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

bool GUID::operator<(const GUID& other) const {
    if (high_ != other.high_) {
        return high_ < other.high_;
    }

    return low_ < other.low_;
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

DateTime::DateTime()
    : secondsSinceEpoch_(std::chrono::duration<double>(
          std::chrono::system_clock::now().time_since_epoch()).count()) {
}

DateTime::DateTime(double secondsSinceEpoch) : secondsSinceEpoch_(secondsSinceEpoch) {
}

DateTime DateTime::fromMillisecondsSinceEpoch(uint64_t millisecondsSinceEpoch) {
    return DateTime(static_cast<double>(millisecondsSinceEpoch) / 1000.0);
}

double DateTime::secondsSinceEpoch() const {
    return secondsSinceEpoch_;
}

uint64_t DateTime::millisecondsSinceEpoch() const {
    const double milliseconds = secondsSinceEpoch_ * 1000.0;
    return static_cast<uint64_t>(std::llround(milliseconds));
}

DateTime& DateTime::operator+=(double seconds) {
    secondsSinceEpoch_ += seconds;
    return *this;
}

DateTime& DateTime::operator+=(const DateTime& other) {
    return (*this += other.secondsSinceEpoch_);
}

bool DateTime::operator==(const DateTime& other) const {
    return secondsSinceEpoch_ == other.secondsSinceEpoch_;
}

bool DateTime::operator!=(const DateTime& other) const {
    return !(*this == other);
}

bool DateTime::operator<(const DateTime& other) const {
    return secondsSinceEpoch_ < other.secondsSinceEpoch_;
}

bool DateTime::operator<=(const DateTime& other) const {
    return secondsSinceEpoch_ <= other.secondsSinceEpoch_;
}

bool DateTime::operator>(const DateTime& other) const {
    return secondsSinceEpoch_ > other.secondsSinceEpoch_;
}

bool DateTime::operator>=(const DateTime& other) const {
    return secondsSinceEpoch_ >= other.secondsSinceEpoch_;
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