#include "common/endorsement/endorsement.hpp"

#include <cstddef>
#include <stdexcept>

namespace rsp {

namespace {

std::runtime_error makeError(const char* message) {
    return std::runtime_error(message);
}

void appendUint16(Buffer& buffer, uint32_t& offset, uint16_t value) {
    buffer.data()[offset++] = static_cast<uint8_t>((value >> 8) & 0xFFU);
    buffer.data()[offset++] = static_cast<uint8_t>(value & 0xFFU);
}

void appendUint32(Buffer& buffer, uint32_t& offset, uint32_t value) {
    buffer.data()[offset++] = static_cast<uint8_t>((value >> 24) & 0xFFU);
    buffer.data()[offset++] = static_cast<uint8_t>((value >> 16) & 0xFFU);
    buffer.data()[offset++] = static_cast<uint8_t>((value >> 8) & 0xFFU);
    buffer.data()[offset++] = static_cast<uint8_t>(value & 0xFFU);
}

void appendUint64(Buffer& buffer, uint32_t& offset, uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        buffer.data()[offset++] = static_cast<uint8_t>((value >> shift) & 0xFFULL);
    }
}

void appendGuid(Buffer& buffer, uint32_t& offset, const GUID& guid) {
    appendUint64(buffer, offset, guid.high());
    appendUint64(buffer, offset, guid.low());
}

void appendBytes(Buffer& buffer, uint32_t& offset, const Buffer& value) {
    if (!value.empty()) {
        std::copy_n(value.data(), value.size(), buffer.data() + offset);
        offset += value.size();
    }
}

uint16_t readUint16(const Buffer& buffer, uint32_t& offset) {
    if (offset + 2 > buffer.size()) {
        throw makeError("buffer underflow while reading uint16");
    }

    const uint16_t value = static_cast<uint16_t>(buffer.data()[offset] << 8) |
                           static_cast<uint16_t>(buffer.data()[offset + 1]);
    offset += 2;
    return value;
}

uint32_t readUint32(const Buffer& buffer, uint32_t& offset) {
    if (offset + 4 > buffer.size()) {
        throw makeError("buffer underflow while reading uint32");
    }

    uint32_t value = 0;
    for (int index = 0; index < 4; ++index) {
        value = (value << 8) | static_cast<uint32_t>(buffer.data()[offset + index]);
    }

    offset += 4;
    return value;
}

uint64_t readUint64(const Buffer& buffer, uint32_t& offset) {
    if (offset + 8 > buffer.size()) {
        throw makeError("buffer underflow while reading uint64");
    }

    uint64_t value = 0;
    for (int index = 0; index < 8; ++index) {
        value = (value << 8) | static_cast<uint64_t>(buffer.data()[offset + index]);
    }

    offset += 8;
    return value;
}

GUID readGuid(const Buffer& buffer, uint32_t& offset) {
    const uint64_t high = readUint64(buffer, offset);
    const uint64_t low = readUint64(buffer, offset);
    return GUID(high, low);
}

Buffer readBytes(const Buffer& buffer, uint32_t& offset, uint32_t length) {
    if (offset + length > buffer.size()) {
        throw makeError("buffer underflow while reading byte array");
    }

    Buffer value(length);
    if (length != 0) {
        std::copy_n(buffer.data() + offset, length, value.data());
        offset += length;
    }

    return value;
}

}  // namespace

Endorsement::Endorsement() : validUntil_(0) {
}

Endorsement::Endorsement(NodeID subject, NodeID endorsementService, GUID endorsementType,
                                                 Buffer endorsementValue, DateTime validUntil, Buffer signature)
    : subject_(subject),
      endorsementService_(endorsementService),
      endorsementType_(endorsementType),
      endorsementValue_(std::move(endorsementValue)),
            validUntil_(std::move(validUntil)),
      signature_(std::move(signature)) {
}

Endorsement Endorsement::createSigned(const KeyPair& endorsementServiceKeyPair, const NodeID& subject,
                                      const GUID& endorsementType, const Buffer& endorsementValue,
                                                                            const DateTime& validUntil) {
    Endorsement endorsement(subject, endorsementServiceKeyPair.nodeID(), endorsementType,
                            endorsementValue, validUntil, Buffer());
    endorsement.signature_ = endorsementServiceKeyPair.sign(endorsement.serializeUnsigned());
    return endorsement;
}

Endorsement Endorsement::deserialize(const Buffer& serialized) {
    uint32_t offset = 0;
    if (readUint32(serialized, offset) != kMagic) {
        throw makeError("endorsement magic mismatch");
    }

    const NodeID subject(readGuid(serialized, offset));
    const NodeID endorsementService(readGuid(serialized, offset));
    const GUID endorsementType(readGuid(serialized, offset));
    const uint32_t valueLength = readUint32(serialized, offset);
    Buffer endorsementValue = readBytes(serialized, offset, valueLength);
    const DateTime validUntil = DateTime::fromMillisecondsSinceEpoch(readUint64(serialized, offset));
    const uint32_t signatureLength = readUint32(serialized, offset);
    Buffer signature = readBytes(serialized, offset, signatureLength);

    if (offset != serialized.size()) {
        throw makeError("extra trailing bytes in endorsement buffer");
    }

    return Endorsement(subject, endorsementService, endorsementType,
                       std::move(endorsementValue), validUntil, std::move(signature));
}

Buffer Endorsement::serialize() const {
    Buffer unsignedPayload = serializeUnsigned();
    const uint32_t totalSize = unsignedPayload.size() + 4 + signature_.size();
    Buffer serialized(totalSize);

    uint32_t offset = 0;
    appendBytes(serialized, offset, unsignedPayload);
    appendUint32(serialized, offset, signature_.size());
    appendBytes(serialized, offset, signature_);
    return serialized;
}

bool Endorsement::verifySignature(const KeyPair& endorsementServiceKeyPair) const {
    if (endorsementServiceKeyPair.nodeID() != endorsementService_) {
        return false;
    }

    return endorsementServiceKeyPair.verify(serializeUnsigned(), signature_);
}

const NodeID& Endorsement::subject() const {
    return subject_;
}

const NodeID& Endorsement::endorsementService() const {
    return endorsementService_;
}

const GUID& Endorsement::endorsementType() const {
    return endorsementType_;
}

const Buffer& Endorsement::endorsementValue() const {
    return endorsementValue_;
}

const DateTime& Endorsement::validUntil() const {
    return validUntil_;
}

const Buffer& Endorsement::signature() const {
    return signature_;
}

Buffer Endorsement::serializeUnsigned() const {
    const uint32_t totalSize = 4 + 16 + 16 + 16 + 4 + endorsementValue_.size() + 8;
    Buffer serialized(totalSize);

    uint32_t offset = 0;
    appendUint32(serialized, offset, kMagic);
    appendGuid(serialized, offset, subject_);
    appendGuid(serialized, offset, endorsementService_);
    appendGuid(serialized, offset, endorsementType_);
    appendUint32(serialized, offset, endorsementValue_.size());
    appendBytes(serialized, offset, endorsementValue_);
    appendUint64(serialized, offset, validUntil_.millisecondsSinceEpoch());
    return serialized;
}

}  // namespace rsp