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

}  // namespace rsp