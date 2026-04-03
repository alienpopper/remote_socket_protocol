#include "common/message_queue.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

int messageValue(const rsp::proto::RSPMessage& message);

class TestMessageQueue : public rsp::MessageQueue {
public:
    std::atomic<bool> fullCallbackTriggered = false;
    std::atomic<int> processedCount = 0;
    std::mutex processedMutex;
    std::vector<int> processedValues;

protected:
    void handleMessage(Message message, rsp::MessageQueueSharedState& sharedState) override {
        const int value = messageValue(message);

        sharedState.update<int>("sum", [value](int& sum) {
            sum += value;
        });

        {
            std::lock_guard<std::mutex> lock(processedMutex);
            processedValues.push_back(value);
        }

        ++processedCount;
    }

    void handleQueueFull(size_t currentSize, size_t limit, const Message&) override {
        if (currentSize == limit && limit != 0) {
            fullCallbackTriggered = true;
        }
    }
};

class PassiveTestQueue : public rsp::MessageQueue {
protected:
    void handleMessage(Message, rsp::MessageQueueSharedState&) override {
    }

    void handleQueueFull(size_t, size_t, const Message&) override {
    }
};

rsp::proto::RSPMessage makeMessage(int value) {
    rsp::proto::RSPMessage message;
    message.mutable_signature()->assign(reinterpret_cast<const char*>(&value), sizeof(value));
    return message;
}

int messageValue(const rsp::proto::RSPMessage& message) {
    int value = 0;
    const std::string& signature = message.signature();
    if (signature.size() == sizeof(value)) {
        std::memcpy(&value, signature.data(), sizeof(value));
    }

    return value;
}

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

void testManualQueueOperations() {
    PassiveTestQueue queue;
    require(queue.push(makeMessage(42)), "manual queue should accept a message");
    require(queue.size() == 1, "manual queue should report queued items");

    rsp::proto::RSPMessage value;
    require(queue.tryPop(value), "manual queue should allow popping a message");
    require(messageValue(value) == 42, "manual queue should preserve the queued value");
    require(queue.size() == 0, "manual queue should be empty after the pop");
}

void testQueueLimitAndFullCallback() {
    TestMessageQueue queue;

    queue.setQueueLimit(1);

    require(queue.push(makeMessage(1)), "queue should accept the first item under the limit");
    require(!queue.push(makeMessage(2)), "queue should reject messages when it is full");
    require(queue.fullCallbackTriggered.load(), "queue should notify when it is full");

    queue.setQueueLimit(2);
    require(queue.push(makeMessage(3)), "queue should accept messages after increasing the queue limit");
}

void testWorkerLifecycleAndSharedState() {
    TestMessageQueue queue;

    queue.sharedState().set<int>("sum", 0);
    queue.setWorkerCount(2);

    queue.start();
    require(queue.isRunning(), "queue should report running after start");

    for (int value = 1; value <= 4; ++value) {
        require(queue.push(makeMessage(value)), "running queue should accept work for the worker pool");
    }

    require(waitForCondition([&queue]() { return queue.processedCount.load() == 4; }),
            "workers should process queued messages");

    const std::optional<int> sum = queue.sharedState().get<int>("sum");
    require(sum.has_value() && *sum == 10, "shared state should be visible to all workers");

    queue.pause();
    require(queue.isPaused(), "queue should report paused after pause");
    require(queue.push(makeMessage(10)), "paused queue should still accept messages");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    require(queue.processedCount.load() == 4, "paused queue should not process additional messages");

    queue.resume();
    require(waitForCondition([&queue]() { return queue.processedCount.load() == 5; }),
            "queue should resume worker processing after resume");

    {
        std::lock_guard<std::mutex> lock(queue.processedMutex);
        require(!queue.processedValues.empty() && queue.processedValues.back() == 10,
                "queue should preserve the paused message across resume");
    }

    queue.setWorkerCount(4);
    for (int value = 11; value <= 14; ++value) {
        require(queue.push(makeMessage(value)), "queue should keep accepting work after worker count changes");
    }

    require(waitForCondition([&queue]() { return queue.processedCount.load() == 9; }),
            "queue should continue processing after changing the worker count");

    queue.stop();
    require(!queue.isRunning(), "queue should report stopped after stop");

    require(queue.push(makeMessage(100)), "stopped queue should retain queued items for a future restart");
    queue.start();
    require(waitForCondition([&queue]() { return queue.processedCount.load() == 10; }),
            "queue should process queued items after restarting");

    queue.stop();
}

}  // namespace

int main() {
    try {
        testManualQueueOperations();
        testQueueLimitAndFullCallback();
        testWorkerLifecycleAndSharedState();

        std::cout << "message_queue_test passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "message_queue_test failed: " << exception.what() << '\n';
        return 1;
    }
}