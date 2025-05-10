#pragma once

#include <string>
#include <functional>
#include <unordered_map>
#include <vector>
#include <mutex>

namespace zm {

// Thread-safe in-process publish/subscribe bus
class EventBus {
public:
    using Callback = std::function<void(const std::string&)>;

    // Get singleton instance
    static EventBus& instance() {
        static EventBus bus;
        return bus;
    }

    // Subscribe a callback to a channel
    void subscribe(const std::string& channel, Callback cb) {
        std::lock_guard<std::mutex> lock(mutex_);
        subscribers_[channel].push_back(std::move(cb));
    }

    // Publish a message to a channel
    void publish(const std::string& channel, const std::string& message) {
        std::vector<Callback> toCall;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = subscribers_.find(channel);
            if (it != subscribers_.end()) {
                toCall = it->second;
            }
        }
        for (auto& cb : toCall) {
            cb(message);
        }
    }

private:
    EventBus() = default;
    ~EventBus() = default;
    EventBus(const EventBus&) = delete;
    EventBus& operator=(const EventBus&) = delete;

    std::unordered_map<std::string, std::vector<Callback>> subscribers_;
    std::mutex mutex_;
};

} // namespace zm
