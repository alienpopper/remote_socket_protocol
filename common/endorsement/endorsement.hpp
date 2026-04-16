#pragma once

#include "common/base_types.hpp"
#include "common/keypair.hpp"
#include "messages.pb.h"

#include <cstdint>
#include <vector>

namespace rsp::resource_manager {
class SchemaSnapshot;
}

namespace rsp {

class Endorsement {
public:
    Endorsement();
    Endorsement(NodeID subject, NodeID endorsementService, GUID endorsementType,
                Buffer endorsementValue, DateTime validUntil, Buffer signature);

    static Endorsement createSigned(const KeyPair& endorsementServiceKeyPair, const NodeID& subject,
                                    const GUID& endorsementType, const Buffer& endorsementValue,
                                    const DateTime& validUntil);
    static Endorsement fromProto(const rsp::proto::Endorsement& message);
    static Endorsement deserialize(const Buffer& serialized);

    Buffer serialize() const;
    rsp::proto::Endorsement toProto() const;
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

bool messageMatchesRequirement(const rsp::proto::ERDAbstractSyntaxTree& tree,
                               const rsp::proto::RSPMessage& message,
                               const rsp::resource_manager::SchemaSnapshot* schemaSnapshot = nullptr);

rsp::proto::ERDAbstractSyntaxTree reduceRequirementTree(const rsp::proto::ERDAbstractSyntaxTree& tree,
                                                        const std::vector<Endorsement>& endorsements,
                                                        const rsp::proto::RSPMessage* message = nullptr,
                                                        const rsp::resource_manager::SchemaSnapshot* schemaSnapshot = nullptr);

}  // namespace rsp