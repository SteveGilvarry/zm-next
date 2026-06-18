// Pure helper for building MQTT topics from event types.
// Header-only so it can be unit-tested without linking libmosquitto.
#pragma once

#include <string>

namespace zm::outputmqtt {

// Build the publish topic for an event of the given type.
// Falls back to "<base>/event" when `type` is empty.
inline std::string topic_for(const std::string& base, const std::string& type) {
    if (type.empty())
        return base + "/event";
    return base + "/" + type;
}

}  // namespace zm::outputmqtt
