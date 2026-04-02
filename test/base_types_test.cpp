#include "common/base_types.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

}  // namespace

int main() {
    try {
    const uint8_t initialBytes[] = {1, 2, 3, 4};
    rsp::Buffer buffer(initialBytes, 4);
    require(buffer.size() == 4, "buffer should report its initial size");
    require(buffer.data()[0] == 1 && buffer.data()[3] == 4,
        "buffer should copy input bytes");

    rsp::Buffer copiedBuffer(buffer);
    require(copiedBuffer.size() == 4, "copied buffer should preserve size");
    require(copiedBuffer.data()[1] == 2, "copied buffer should preserve contents");

    buffer.resize(6);
    require(buffer.size() == 6, "buffer resize should update size");
    require(buffer.data()[0] == 1 && buffer.data()[3] == 4,
        "buffer resize should preserve existing contents");

        const rsp::GUID randomGuid;
        const std::string randomGuidText = randomGuid.toString();
        require(randomGuidText.size() == 36,
            "default GUID should serialize to canonical GUID text length");

        const rsp::GUID reparsedRandomGuid(randomGuidText);
        require(reparsedRandomGuid == randomGuid,
            "default GUID should round-trip through canonical text");

        const rsp::DateTime now;
        require(now.secondsSinceEpoch() > YEARS(40),
            "default DateTime should initialize to a plausible current epoch time");

        rsp::DateTime adjustedTime(100.0);
        adjustedTime += HOURS(2);
        require(std::fabs(adjustedTime.secondsSinceEpoch() - (100.0 + HOURS(2))) < 0.000001,
            "DateTime += double should adjust the stored seconds");

        adjustedTime += rsp::DateTime(30.0);
        require(std::fabs(adjustedTime.secondsSinceEpoch() - (100.0 + HOURS(2) + 30.0)) < 0.000001,
            "DateTime += DateTime should adjust by the other instance's seconds");

        require(rsp::DateTime(10.0) < rsp::DateTime(20.0),
            "DateTime comparisons should order by epoch seconds");

        const rsp::GUID parsedGuid("00112233-4455-6677-8899-aabbccddeeff");
        require(parsedGuid.toString() == "00112233-4455-6677-8899-aabbccddeeff",
                "GUID string round-trip should preserve canonical format");

        const rsp::GUID braceGuid("{00112233-4455-6677-8899-aabbccddeeff}");
        require(parsedGuid == braceGuid, "brace-delimited GUID should parse identically");

        const rsp::GUID splitGuid(0x0011223344556677ULL, 0x8899AABBCCDDEEFFULL);
        require(splitGuid.toString() == "00112233-4455-6677-8899-aabbccddeeff",
                "high/low constructor should serialize to canonical GUID text");

        const std::string convertedGuid = static_cast<std::string>(splitGuid);
        require(convertedGuid == "00112233-4455-6677-8899-aabbccddeeff",
                "GUID should implicitly convert to std::string");

        const rsp::NodeID nodeId(parsedGuid);
        require(nodeId.toString() == "00112233-4455-6677-8899-aabbccddeeff",
            "NodeID should preserve GUID string formatting");

        const std::string convertedNodeId = static_cast<std::string>(nodeId);
        require(convertedNodeId == "00112233-4455-6677-8899-aabbccddeeff",
            "NodeID should implicitly convert to std::string");

        bool invalidThrown = false;
        try {
            static_cast<void>(rsp::GUID("not-a-guid"));
        } catch (const std::invalid_argument&) {
            invalidThrown = true;
        }

        require(invalidThrown, "invalid GUID text should throw std::invalid_argument");

        std::cout << "base_types_test passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "base_types_test failed: " << exception.what() << '\n';
        return 1;
    }
}