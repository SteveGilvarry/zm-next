// Implementation for EventBus
#include "zm/EventBus.hpp"

namespace zm {

// Implement publish for plugin API: forwards to std::string overload
bool EventBus::publish(const char* topic, const char* payload) {
    this->publish(std::string(topic), std::string(payload));
    return true;
}

} // namespace zm
