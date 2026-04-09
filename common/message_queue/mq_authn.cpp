#include "common/message_queue/mq_authn.hpp"

#include "common/base_types.hpp"
#include "os/os_random.hpp"

#include <optional>
#include <stdexcept>
#include <string>

namespace rsp::message_queue {

namespace {

constexpr uint32_t kNonceLength = 16;

rsp::Buffer serializeUnsignedMessage(const rsp::proto::RSPMessage& message) {
    rsp::proto::RSPMessage unsignedMessage = message;
    unsignedMessage.clear_signature();

    std::string payload;
    if (!unsignedMessage.SerializeToString(&payload)) {
        throw std::runtime_error("failed to serialize unsigned authentication message");
    }

    if (payload.empty()) {
        return rsp::Buffer();
    }

    return rsp::Buffer(reinterpret_cast<const uint8_t*>(payload.data()), static_cast<uint32_t>(payload.size()));
}

std::string randomNonceBytes() {
    std::string nonce(kNonceLength, '\0');
    rsp::os::randomFill(reinterpret_cast<uint8_t*>(nonce.data()), kNonceLength);
    return nonce;
}

bool validateReceivedIdentity(const rsp::proto::RSPMessage& identityMessage,
                              const std::string& expectedNonce,
                              rsp::NodeID& peerNodeId) {
    if (!identityMessage.has_identity() || !identityMessage.has_signature()) {
        return false;
    }

    if (identityMessage.identity().nonce().value() != expectedNonce) {
        return false;
    }

    try {
        rsp::KeyPair peerKey = rsp::KeyPair::fromPublicKey(identityMessage.identity().public_key());
        if (!peerKey.verifyBlock(serializeUnsignedMessage(identityMessage), identityMessage.signature())) {
            return false;
        }

        peerNodeId = peerKey.nodeID();
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

}  // namespace

MessageQueueAuthN::MessageQueueAuthN(rsp::KeyPair keyPair,
                                     SuccessCallback success,
                                     FailureCallback failure,
                                     StoreIdentityCallback storeIdentity)
    : keyPair_(std::move(keyPair)),
      success_(std::move(success)),
      failure_(std::move(failure)),
      storeIdentity_(std::move(storeIdentity)) {
}

bool MessageQueueAuthN::performInitialIdentityExchange(rsp::encoding::Encoding& encoding) const {
    const auto activeConnection = encoding.connection();
    if (activeConnection == nullptr || !keyPair_.isValid()) {
        return false;
    }

    rsp::proto::RSPMessage challengeRequest;
    challengeRequest.mutable_challenge_request()->mutable_nonce()->set_value(randomNonceBytes());
    const std::string localChallengeNonce = challengeRequest.challenge_request().nonce().value();

    {
        std::lock_guard<std::mutex> lock(encoding.sendMutex_);
        if (!encoding.writeMessage(encoding.normalizeOutgoingMessage(std::move(challengeRequest)))) {
            return false;
        }
    }

    bool peerIdentityReceived = false;
    bool localIdentitySent = false;
    bool peerChallengeReceived = false;

    while (!peerIdentityReceived || !localIdentitySent) {
        rsp::proto::RSPMessage incomingMessage;
        if (!encoding.readMessage(incomingMessage)) {
            return false;
        }

        if (incomingMessage.has_challenge_request()) {
            if (incomingMessage.has_destination() || incomingMessage.has_signature() || peerChallengeReceived ||
                incomingMessage.challenge_request().nonce().value().size() != kNonceLength) {
                return false;
            }

            rsp::proto::RSPMessage identityMessage;
            identityMessage.mutable_identity()->mutable_nonce()->CopyFrom(incomingMessage.challenge_request().nonce());
            *identityMessage.mutable_identity()->mutable_public_key() = keyPair_.publicKey();
            *identityMessage.mutable_signature() = keyPair_.signBlock(serializeUnsignedMessage(identityMessage));

            {
                std::lock_guard<std::mutex> lock(encoding.sendMutex_);
                if (!encoding.writeMessage(identityMessage)) {
                    return false;
                }
            }

            peerChallengeReceived = true;
            localIdentitySent = true;
            continue;
        }

        if (incomingMessage.has_identity()) {
            if (peerIdentityReceived) {
                return false;
            }

            rsp::NodeID peerNodeId(0, 0);
            if (!validateReceivedIdentity(incomingMessage, localChallengeNonce, peerNodeId)) {
                return false;
            }

            encoding.setPeerNodeID(peerNodeId);
            if (storeIdentity_) {
                storeIdentity_(incomingMessage.identity());
            }
            peerIdentityReceived = true;
            continue;
        }

        return false;
    }

    return true;
}

void MessageQueueAuthN::handleMessage(Message encoding, rsp::MessageQueueSharedState&) {
    if (encoding == nullptr || !performInitialIdentityExchange(*encoding)) {
        if (failure_) {
            failure_(encoding);
        }
        return;
    }

    if (success_) {
        success_(encoding);
    }
}

void MessageQueueAuthN::handleQueueFull(size_t, size_t, const Message& encoding) {
    if (failure_) {
        failure_(encoding);
    }
}

}  // namespace rsp::message_queue