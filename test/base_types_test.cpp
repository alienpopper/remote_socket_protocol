#include "common/base_types.hpp"

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
        const rsp::GUID emptyGuid;
        require(emptyGuid.toString() == "00000000-0000-0000-0000-000000000000",
                "default GUID should be zeroed");

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