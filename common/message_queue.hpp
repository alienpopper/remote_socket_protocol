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

template <typename MessageT>
class MessageQueue {
public:
    using Message = MessageT;

    MessageQueue()
        : queueLimit_(0), workerCount_(0), running_(false), paused_(false), stopRequested_(false) {
    }

    virtual ~MessageQueue() {
        stop();
    }

    MessageQueue(const MessageQueue&) = delete;
    MessageQueue& operator=(const MessageQueue&) = delete;

    bool push(Message message) {
        size_t currentSize = 0;
        size_t currentLimit = 0;
        bool isFull = false;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            currentSize = messages_.size();
            currentLimit = queueLimit_;
            if (queueLimit_ != 0 && currentSize >= queueLimit_) {
                isFull = true;
            } else {
                messages_.push_back(std::move(message));
                condition_.notify_one();
                return true;
            }
        }

        if (isFull) {
            handleQueueFull(currentSize, currentLimit, message);
        }

        return false;
    }

    bool tryPop(Message& message) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (messages_.empty()) {
            return false;
        }

        message = std::move(messages_.front());
        messages_.pop_front();
        return true;
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return messages_.size();
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        messages_.clear();
    }

    void setQueueLimit(size_t limit) {
        std::lock_guard<std::mutex> lock(mutex_);
        queueLimit_ = limit;
    }

    size_t queueLimit() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queueLimit_;
    }

    void setWorkerCount(size_t workerCount) {
        bool wasRunning = false;
        bool wasPaused = false;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (workerCount_ == workerCount) {
                return;
            }

            workerCount_ = workerCount;
            wasRunning = running_;
            wasPaused = paused_;
        }

        rebuildWorkers(workerCount, wasRunning, wasPaused);
    }

    size_t workerCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return workerCount_;
    }

    void start() {
        bool shouldStartWorkers = false;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!running_) {
                running_ = true;
                stopRequested_ = false;
                shouldStartWorkers = true;
            }

            paused_ = false;
        }

        if (shouldStartWorkers) {
            std::lock_guard<std::mutex> lock(mutex_);
            startWorkersLocked();
        }

        condition_.notify_all();
    }

    void pause() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            paused_ = true;
        }

        condition_.notify_all();
    }

    void resume() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            paused_ = false;
        }

        condition_.notify_all();
    }

    void stop() {
        std::vector<std::thread> workers;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!running_ && workers_.empty()) {
                paused_ = false;
                stopRequested_ = false;
                return;
            }

            running_ = false;
            paused_ = false;
            stopRequested_ = true;
            workers.swap(workers_);
        }

        condition_.notify_all();

        for (std::thread& worker : workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }

        std::lock_guard<std::mutex> lock(mutex_);
        stopRequested_ = false;
    }

    bool isRunning() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return running_;
    }

    bool isPaused() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return paused_;
    }

    MessageQueueSharedState& sharedState() {
        return sharedState_;
    }

    const MessageQueueSharedState& sharedState() const {
        return sharedState_;
    }

protected:
    virtual void handleMessage(Message message, MessageQueueSharedState& sharedState) = 0;
    virtual void handleQueueFull(size_t currentSize, size_t limit, const Message& rejectedMessage) = 0;

private:
    void startWorkersLocked() {
        if (!workers_.empty()) {
            return;
        }

        workers_.reserve(workerCount_);
        for (size_t workerIndex = 0; workerIndex < workerCount_; ++workerIndex) {
            workers_.emplace_back(&MessageQueue::workerLoop, this);
        }
    }

    void workerLoop() {
        while (true) {
            Message message;

            {
                std::unique_lock<std::mutex> lock(mutex_);
                condition_.wait(lock, [this]() {
                    return stopRequested_ || (running_ && !paused_ && !messages_.empty());
                });

                if (stopRequested_) {
                    return;
                }

                if (!running_ || paused_ || messages_.empty()) {
                    continue;
                }

                message = std::move(messages_.front());
                messages_.pop_front();
            }

            handleMessage(std::move(message), sharedState_);
        }
    }

    void rebuildWorkers(size_t workerCount, bool wasRunning, bool wasPaused) {
        if (!wasRunning) {
            return;
        }

        std::vector<std::thread> workers;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopRequested_ = true;
            workers.swap(workers_);
        }

        condition_.notify_all();

        for (std::thread& worker : workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopRequested_ = false;
            running_ = true;
            paused_ = wasPaused;
            workerCount_ = workerCount;
            startWorkersLocked();
        }

        condition_.notify_all();
    }

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

template <typename MessageT>
class BufferedQueue : public MessageQueue<MessageT> {
public:
    using Message = typename MessageQueue<MessageT>::Message;

    ~BufferedQueue() override {
        this->stop();
    }

protected:
    void handleMessage(Message, MessageQueueSharedState&) override {
    }

    void handleQueueFull(size_t, size_t, const Message&) override {
    }
};

using RSPMessageQueue = MessageQueue<rsp::proto::RSPMessage>;
using BufferedMessageQueue = BufferedQueue<rsp::proto::RSPMessage>;
using MessageQueueHandle = std::shared_ptr<RSPMessageQueue>;

template <typename MessageT>
using MessageQueueHandleT = std::shared_ptr<MessageQueue<MessageT>>;

}  // namespace rsp