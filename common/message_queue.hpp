#pragma once

#include "messages.pb.h"

#include <any>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rsp {

class MessageQueueSharedState {
public:
    template <typename T>
    void set(const std::string& key, T value) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        values_[key] = std::any(std::move(value));
    }

    template <typename T>
    std::optional<T> get(const std::string& key) const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        const auto iterator = values_.find(key);
        if (iterator == values_.end()) {
            return std::nullopt;
        }

        const T* typedValue = std::any_cast<T>(&(iterator->second));
        if (typedValue == nullptr) {
            return std::nullopt;
        }

        return *typedValue;
    }

    template <typename T, typename Updater>
    bool update(const std::string& key, Updater&& updater) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        const auto iterator = values_.find(key);
        if (iterator == values_.end()) {
            return false;
        }

        T* typedValue = std::any_cast<T>(&(iterator->second));
        if (typedValue == nullptr) {
            return false;
        }

        std::forward<Updater>(updater)(*typedValue);
        return true;
    }

    bool contains(const std::string& key) const;
    void erase(const std::string& key);

private:
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, std::any> values_;
};

class MessageQueue {
public:
    using Message = rsp::proto::RSPMessage;

    MessageQueue();
    virtual ~MessageQueue();

    MessageQueue(const MessageQueue&) = delete;
    MessageQueue& operator=(const MessageQueue&) = delete;

    bool push(Message message);
    bool tryPop(Message& message);
    size_t size() const;
    void clear();

    void setQueueLimit(size_t limit);
    size_t queueLimit() const;

    void setWorkerCount(size_t workerCount);
    size_t workerCount() const;

    void start();
    void pause();
    void resume();
    void stop();

    bool isRunning() const;
    bool isPaused() const;

    MessageQueueSharedState& sharedState();
    const MessageQueueSharedState& sharedState() const;

protected:
    virtual void handleMessage(Message message, MessageQueueSharedState& sharedState) = 0;
    virtual void handleQueueFull(size_t currentSize, size_t limit, const Message& rejectedMessage) = 0;

private:
    void startWorkersLocked();
    void workerLoop();
    void rebuildWorkers(size_t workerCount, bool wasRunning, bool wasPaused);

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<Message> messages_;
    size_t queueLimit_;
    size_t workerCount_;
    bool running_;
    bool paused_;
    bool stopRequested_;
    std::vector<std::thread> workers_;
    MessageQueueSharedState sharedState_;
};

class BufferedMessageQueue : public MessageQueue {
public:
    ~BufferedMessageQueue() override;

protected:
    void handleMessage(Message message, MessageQueueSharedState& sharedState) override;
    void handleQueueFull(size_t currentSize, size_t limit, const Message& rejectedMessage) override;
};

using MessageQueueHandle = std::shared_ptr<MessageQueue>;

}  // namespace rsp