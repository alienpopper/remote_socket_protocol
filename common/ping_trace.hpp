#pragma once

#include "messages.pb.h"

#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace rsp::ping_trace {

using Clock = std::chrono::steady_clock;

struct TraceEvent {
    std::string name;
    Clock::time_point timestamp;
};

struct TraceSnapshot {
    std::string nonce;
    std::vector<TraceEvent> events;
};

inline std::mutex& traceMutex() {
    static std::mutex mutex;
    return mutex;
}

inline bool& tracingEnabled() {
    static bool enabled = false;
    return enabled;
}

inline std::unordered_map<std::string, std::vector<TraceEvent>>& traces() {
    static std::unordered_map<std::string, std::vector<TraceEvent>> activeTraces;
    return activeTraces;
}

inline std::optional<std::string> nonceForMessage(const rsp::proto::RSPMessage& message) {
    if (message.has_ping_request() && message.ping_request().has_nonce()) {
        return message.ping_request().nonce().value();
    }

    if (message.has_ping_reply() && message.ping_reply().has_nonce()) {
        return message.ping_reply().nonce().value();
    }

    return std::nullopt;
}

inline void reset() {
    std::lock_guard<std::mutex> lock(traceMutex());
    traces().clear();
}

inline void setEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(traceMutex());
    tracingEnabled() = enabled;
}

inline bool isEnabled() {
    std::lock_guard<std::mutex> lock(traceMutex());
    return tracingEnabled();
}

inline bool start(const std::string& nonce) {
    std::lock_guard<std::mutex> lock(traceMutex());
    if (!tracingEnabled()) {
        return false;
    }

    traces()[nonce] = {};
    traces()[nonce].push_back(TraceEvent{"trace_started", Clock::now()});
    return true;
}

inline void record(const std::string& nonce, const std::string& eventName) {
    if (nonce.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(traceMutex());
    const auto iterator = traces().find(nonce);
    if (iterator == traces().end()) {
        return;
    }

    iterator->second.push_back(TraceEvent{eventName, Clock::now()});
}

inline void recordForMessage(const rsp::proto::RSPMessage& message, const std::string& eventName) {
    const auto nonce = nonceForMessage(message);
    if (!nonce.has_value()) {
        return;
    }

    record(*nonce, eventName);
}

inline std::vector<TraceSnapshot> snapshotAll() {
    std::lock_guard<std::mutex> lock(traceMutex());
    std::vector<TraceSnapshot> snapshots;
    snapshots.reserve(traces().size());
    for (const auto& [nonce, events] : traces()) {
        snapshots.push_back(TraceSnapshot{nonce, events});
    }

    return snapshots;
}

}  // namespace rsp::ping_trace