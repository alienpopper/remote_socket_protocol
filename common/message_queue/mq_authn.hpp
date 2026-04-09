#pragma once

#include "common/encoding/encoding.hpp"
#include "common/keypair.hpp"
#include "common/message_queue/mq.hpp"
#include "messages.pb.h"

#include <functional>

namespace rsp::message_queue {

class MessageQueueAuthN : public rsp::MessageQueue<rsp::encoding::EncodingHandle> {
public:
    using SuccessCallback = std::function<void(const rsp::encoding::EncodingHandle& encoding)>;
    using FailureCallback = std::function<void(const rsp::encoding::EncodingHandle& encoding)>;
    using StoreIdentityCallback = std::function<void(const rsp::proto::Identity& identity)>;

    MessageQueueAuthN(rsp::KeyPair keyPair,
                      SuccessCallback success,
                      FailureCallback failure,
                      StoreIdentityCallback storeIdentity);

protected:
    void handleMessage(Message encoding, rsp::MessageQueueSharedState& sharedState) override;
    void handleQueueFull(size_t currentSize, size_t limit, const Message& encoding) override;

private:
    bool performInitialIdentityExchange(rsp::encoding::Encoding& encoding) const;

    rsp::KeyPair keyPair_;
    SuccessCallback success_;
    FailureCallback failure_;
    StoreIdentityCallback storeIdentity_;
};

}  // namespace rsp::message_queue