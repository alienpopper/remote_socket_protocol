#pragma once

#include "common/base_types.hpp"
#include "common/keypair.hpp"
#include "messages.pb.h"

#include <cstdint>
#include <vector>

namespace rsp {

class Endorsement {
public:
    Endorsement();
    Endorsement(NodeID subject, NodeID endorsementService, GUID endorsementType,
                Buffer endorsementValue, DateTime validUntil, Buffer signature);

    static Endorsement createSigned(const KeyPair& endorsementServiceKeyPair, const NodeID& subject,
                                    const GUID& endorsementType, const Buffer& endorsementValue,
                                    const DateTime& validUntil);
    static Endorsement deserialize(const Buffer& serialized);

    Buffer serialize() const;
    bool verifySignature(const KeyPair& endorsementServiceKeyPair) const;

    NodeID subject() const;
    NodeID endorsementService() const;
    GUID endorsementType() const;
    Buffer endorsementValue() const;
    DateTime validUntil() const;
    Buffer signature() const;

private:
    explicit Endorsement(rsp::proto::Endorsement message);

    Buffer serializeUnsigned() const;
    static void validateMessage(const rsp::proto::Endorsement& message, bool requireSignature);

    rsp::proto::Endorsement message_;
};

bool endorsementMatchesRequirement(const rsp::proto::EndorsementNeeded& requirement,
                                  const Endorsement& endorsement);

rsp::proto::ERDAbstractSyntaxTree reduceRequirementTree(const rsp::proto::ERDAbstractSyntaxTree& tree,
                                                        const std::vector<Endorsement>& endorsements);

}  // namespace rsp