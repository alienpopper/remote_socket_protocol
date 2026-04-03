#include "common/message_queue.hpp"

namespace rsp {

bool MessageQueueSharedState::contains(const std::string& key) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return values_.find(key) != values_.end();
}

void MessageQueueSharedState::erase(const std::string& key) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    values_.erase(key);
}

MessageQueue::MessageQueue()
    : queueLimit_(0), workerCount_(0), running_(false), paused_(false), stopRequested_(false) {
}

MessageQueue::~MessageQueue() {
    stop();
}

bool MessageQueue::push(Message message) {
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

bool MessageQueue::tryPop(Message& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (messages_.empty()) {
        return false;
    }

    message = std::move(messages_.front());
    messages_.pop_front();
    return true;
}

size_t MessageQueue::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return messages_.size();
}

void MessageQueue::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    messages_.clear();
}

void MessageQueue::setQueueLimit(size_t limit) {
    std::lock_guard<std::mutex> lock(mutex_);
    queueLimit_ = limit;
}

size_t MessageQueue::queueLimit() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queueLimit_;
}

void MessageQueue::setWorkerCount(size_t workerCount) {
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

size_t MessageQueue::workerCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return workerCount_;
}

void MessageQueue::start() {
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

void MessageQueue::pause() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        paused_ = true;
    }

    condition_.notify_all();
}

void MessageQueue::resume() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        paused_ = false;
    }

    condition_.notify_all();
}

void MessageQueue::stop() {
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

bool MessageQueue::isRunning() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return running_;
}

bool MessageQueue::isPaused() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return paused_;
}

MessageQueueSharedState& MessageQueue::sharedState() {
    return sharedState_;
}

const MessageQueueSharedState& MessageQueue::sharedState() const {
    return sharedState_;
}

void MessageQueue::startWorkersLocked() {
    if (!workers_.empty()) {
        return;
    }

    workers_.reserve(workerCount_);
    for (size_t workerIndex = 0; workerIndex < workerCount_; ++workerIndex) {
        workers_.emplace_back(&MessageQueue::workerLoop, this);
    }
}

void MessageQueue::workerLoop() {
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

void MessageQueue::rebuildWorkers(size_t workerCount, bool wasRunning, bool wasPaused) {
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

BufferedMessageQueue::~BufferedMessageQueue() {
    stop();
}

void BufferedMessageQueue::handleMessage(Message, MessageQueueSharedState&) {
}

void BufferedMessageQueue::handleQueueFull(size_t, size_t, const Message&) {
}

}  // namespace rsp