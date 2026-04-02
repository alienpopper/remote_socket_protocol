#pragma once

#include "common/base_types.hpp"
#include "common/keypair.hpp"

#include <cstdint>

namespace rsp {

class Endorsement {
public:
    static constexpr uint32_t kMagic = 0xC7A4D92E;

    Endorsement();
    Endorsement(NodeID subject, NodeID endorsementService, GUID endorsementType,
                Buffer endorsementValue, DateTime validUntil, Buffer signature);

    static Endorsement createSigned(const KeyPair& endorsementServiceKeyPair, const NodeID& subject,
                                    const GUID& endorsementType, const Buffer& endorsementValue,
                                    const DateTime& validUntil);
    static Endorsement deserialize(const Buffer& serialized);

    Buffer serialize() const;
    bool verifySignature(const KeyPair& endorsementServiceKeyPair) const;

    const NodeID& subject() const;
    const NodeID& endorsementService() const;
    const GUID& endorsementType() const;
    const Buffer& endorsementValue() const;
    const DateTime& validUntil() const;
    const Buffer& signature() const;

private:
    Buffer serializeUnsigned() const;

    NodeID subject_;
    NodeID endorsementService_;
    GUID endorsementType_;
    Buffer endorsementValue_;
    DateTime validUntil_;
    Buffer signature_;
};

}  // namespace rsp