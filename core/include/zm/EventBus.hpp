#pragma once

#include <string>
#include <functional>
#include <unordered_map>
#include <vector>
#include <utility>
#include <algorithm>
#include <cstdint>
#include <mutex>

namespace zm {

// Thread-safe in-process publish/subscribe bus
class EventBus {
public:
    using Callback = std::function<void(const std::string&)>;
    using SubscriptionId = uint64_t;

    // Get singleton instance
    static EventBus& instance() {
        static EventBus bus;
        return bus;
    }

    // Subscribe a callback to a channel. Returns a token to pass to unsubscribe().
    SubscriptionId subscribe(const std::string& channel, Callback cb) {
        std::lock_guard<std::mutex> lock(mutex_);
        const SubscriptionId id = ++lastId_;
        subscribers_[channel].push_back({id, std::move(cb)});
        return id;
    }

    // Remove a previously-registered callback. Safe to call even if a publish is
    // in flight: publish() invokes copies outside the lock, so an in-flight
    // callback completes; this only prevents FUTURE deliveries.
    void unsubscribe(const std::string& channel, SubscriptionId id) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = subscribers_.find(channel);
        if (it == subscribers_.end()) return;
        auto& vec = it->second;
        vec.erase(std::remove_if(vec.begin(), vec.end(),
                                 [id](const auto& p) { return p.first == id; }),
                  vec.end());
    }

    // Publish a message to a channel
    void publish(const std::string& channel, const std::string& message) {
        std::vector<Callback> toCall;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = subscribers_.find(channel);
            if (it != subscribers_.end()) {
                toCall.reserve(it->second.size());
                for (auto& p : it->second) toCall.push_back(p.second);
            }
        }
        for (auto& cb : toCall) {
            cb(message);
        }
    }

    // C API for plugins: publish event (returns true for now)
    bool publish(const char* topic, const char* payload);

private:
    EventBus() = default;
    ~EventBus() = default;
    EventBus(const EventBus&) = delete;
    EventBus& operator=(const EventBus&) = delete;

    std::unordered_map<std::string, std::vector<std::pair<SubscriptionId, Callback>>> subscribers_;
    SubscriptionId lastId_ = 0;
    std::mutex mutex_;
};

} // namespace zm
