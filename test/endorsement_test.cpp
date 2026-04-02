#include "common/endorsement/endorsement.hpp"

#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <iostream>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void testSerializationRoundTrip() {
    rsp::KeyPair endorsementServiceKey = rsp::KeyPair::generateP256();
    rsp::KeyPair subjectKey = rsp::KeyPair::generateP256();

    const uint8_t valueBytes[] = {0x10, 0x20, 0x30, 0x40};
    const rsp::Buffer endorsementValue(valueBytes, 4);
    const rsp::GUID endorsementType("00112233-4455-6677-8899-aabbccddeeff");
    const rsp::DateTime validUntil = rsp::DateTime::fromMillisecondsSinceEpoch(1712083200123ULL);

    const rsp::Endorsement endorsement = rsp::Endorsement::createSigned(
        endorsementServiceKey, subjectKey.nodeID(), endorsementType, endorsementValue, validUntil);

    require(endorsement.subject() == subjectKey.nodeID(), "endorsement should preserve subject");
    require(endorsement.endorsementService() == endorsementServiceKey.nodeID(),
            "endorsement should preserve endorsement service");
    require(endorsement.endorsementType() == endorsementType, "endorsement should preserve type");
    require(endorsement.endorsementValue().size() == 4, "endorsement should preserve value bytes");
    require(endorsement.validUntil().millisecondsSinceEpoch() == validUntil.millisecondsSinceEpoch(),
            "endorsement should preserve expiration");
    require(!endorsement.signature().empty(), "endorsement should contain a signature");

    const rsp::Buffer serialized = endorsement.serialize();
    require(!serialized.empty(), "serialized endorsement should not be empty");

    const rsp::Endorsement reparsed = rsp::Endorsement::deserialize(serialized);
    require(reparsed.subject() == endorsement.subject(), "deserialization should preserve subject");
    require(reparsed.endorsementService() == endorsement.endorsementService(),
            "deserialization should preserve endorsement service");
    require(reparsed.endorsementType() == endorsement.endorsementType(),
            "deserialization should preserve endorsement type");
    require(reparsed.endorsementValue().size() == endorsement.endorsementValue().size(),
            "deserialization should preserve endorsement value size");
    require(reparsed.validUntil().millisecondsSinceEpoch() == endorsement.validUntil().millisecondsSinceEpoch(),
            "deserialization should preserve expiration");
}

void testSignatureVerification() {
    rsp::KeyPair endorsementServiceKey = rsp::KeyPair::generateP256();
    rsp::KeyPair otherKey = rsp::KeyPair::generateP256();
    rsp::KeyPair subjectKey = rsp::KeyPair::generateP256();

    const uint8_t valueBytes[] = {0xAB, 0xCD, 0xEF};
    const rsp::Buffer endorsementValue(valueBytes, 3);
    const rsp::GUID endorsementType("12345678-90ab-cdef-1234-567890abcdef");
    const rsp::DateTime validUntil = rsp::DateTime::fromMillisecondsSinceEpoch(1800000000456ULL);

    const rsp::Endorsement endorsement = rsp::Endorsement::createSigned(
        endorsementServiceKey, subjectKey.nodeID(), endorsementType, endorsementValue, validUntil);

    require(endorsement.verifySignature(endorsementServiceKey),
            "endorsement should verify with the issuing key");
    require(!endorsement.verifySignature(otherKey),
            "endorsement should not verify with a different key");
}

void testTamperingInvalidatesSignature() {
    rsp::KeyPair endorsementServiceKey = rsp::KeyPair::generateP256();
    rsp::KeyPair subjectKey = rsp::KeyPair::generateP256();

    const uint8_t valueBytes[] = {0x01, 0x02, 0x03, 0x04, 0x05};
    const rsp::Buffer endorsementValue(valueBytes, 5);
    const rsp::GUID endorsementType("fedcba98-7654-3210-fedc-ba9876543210");
    const rsp::DateTime validUntil = rsp::DateTime::fromMillisecondsSinceEpoch(1900000000789ULL);

    const rsp::Endorsement endorsement = rsp::Endorsement::createSigned(
        endorsementServiceKey, subjectKey.nodeID(), endorsementType, endorsementValue, validUntil);

    rsp::Buffer tamperedSerialized = endorsement.serialize();
    tamperedSerialized.data()[40] ^= 0x01;

    const rsp::Endorsement tampered = rsp::Endorsement::deserialize(tamperedSerialized);
    require(!tampered.verifySignature(endorsementServiceKey),
            "tampering with the serialized payload should invalidate the signature");
}

void testMalformedBufferRejection() {
    rsp::Buffer malformed(2);
    malformed.data()[0] = 0x00;
    malformed.data()[1] = 0x00;

    bool invalidThrown = false;
    try {
        static_cast<void>(rsp::Endorsement::deserialize(malformed));
    } catch (const std::runtime_error&) {
        invalidThrown = true;
    }

    require(invalidThrown, "deserializing a malformed endorsement should throw");
}

}  // namespace

int main() {
    try {
        testSerializationRoundTrip();
        testSignatureVerification();
        testTamperingInvalidatesSignature();
        testMalformedBufferRejection();

        std::cout << "endorsement_test passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "endorsement_test failed: " << exception.what() << '\n';
        return 1;
    }
}