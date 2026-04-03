#include "common/node.hpp"

#include <chrono>
#include <functional>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

class TestNode : public rsp::RSPNode {
public:
    int run() const override {
        return 0;
    }

protected:
    bool handleNodeSpecificMessage(const rsp::proto::RSPMessage&) override {
        return false;
    }

    void handleOutputMessage(rsp::proto::RSPMessage) override {
    }

public:

    void pauseOutputQueue() {
        outputQueue()->pause();
    }

    bool tryPopOutput(rsp::proto::RSPMessage& message) {
        return outputQueue()->tryPop(message);
    }
};

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

bool waitForCondition(const std::function<bool()>& condition) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition()) {
            return true;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    return condition();
}

rsp::proto::RSPMessage makePingRequest() {
    rsp::proto::RSPMessage message;
    message.mutable_source()->set_value("requester-node");
    message.mutable_ping_request()->mutable_nonce()->set_value("ping-nonce");
    message.mutable_ping_request()->set_sequence(7);
    message.mutable_ping_request()->mutable_time_sent()->set_milliseconds_since_epoch(123456);
    return message;
}

rsp::proto::RSPMessage makeUnsupportedMessage() {
    rsp::proto::RSPMessage message;
    message.mutable_source()->set_value("requester-node");
    message.mutable_route();
    return message;
}

void testPingProducesReply() {
    TestNode node;
    node.pauseOutputQueue();

    require(node.enqueueInput(makePingRequest()), "node should accept a ping request on its input queue");
    require(waitForCondition([&node]() { return node.pendingOutputCount() == 1; }),
            "node should enqueue a ping reply on the output queue");

    rsp::proto::RSPMessage reply;
    require(node.tryPopOutput(reply), "node should allow inspection of the generated ping reply");
    require(reply.has_ping_reply(), "node should produce a ping reply for a ping request");
    require(reply.destination().value() == "requester-node", "ping reply should target the original sender");
    require(reply.ping_reply().nonce().value() == "ping-nonce", "ping reply should preserve the request nonce");
    require(reply.ping_reply().sequence() == 7, "ping reply should preserve the request sequence");
    require(reply.ping_reply().time_sent().milliseconds_since_epoch() == 123456,
            "ping reply should preserve the original send time");
    require(reply.ping_reply().has_time_replied(), "ping reply should stamp a reply time");
}

void testUnsupportedMessageProducesError() {
    TestNode node;
    node.pauseOutputQueue();

    require(node.enqueueInput(makeUnsupportedMessage()), "node should accept unsupported messages on its input queue");
    require(waitForCondition([&node]() { return node.pendingOutputCount() == 1; }),
            "node should enqueue an error reply for unsupported messages");

    rsp::proto::RSPMessage reply;
    require(node.tryPopOutput(reply), "node should allow inspection of the generated error reply");
    require(reply.has_error(), "node should produce an error reply for unsupported messages");
    require(reply.error().error_code() == rsp::proto::UNKNOWN_MESSAGE_TYPE,
            "node should classify unsupported messages as unknown message types");
    require(reply.destination().value() == "requester-node", "error reply should target the original sender");
}

}  // namespace

int main() {
    try {
        testPingProducesReply();
        testUnsupportedMessageProducesError();
        std::cout << "node_test passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "node_test failed: " << exception.what() << '\n';
        return 1;
    }
}
