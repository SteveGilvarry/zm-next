// event_trigger.hpp — pure helper for store_event.
//
// is_trigger() decides whether an incoming event "type" should start/extend an
// event recording. It is intentionally free of any FFmpeg / ABI dependency so it
// can be unit-tested on its own.
//
// Semantics of an EMPTY trigger_types list: it falls back to the built-in
// DEFAULT trigger set ("motion", "detection", "audio_event",
// "tracked_detection"). This mirrors the plugin's documented default config
// value, so a plugin configured with no "trigger_types" key behaves the same as
// one configured with the explicit default array. (To disable triggering
// entirely, configure a list that contains no matching event types, e.g.
// ["none"].)

#pragma once

#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace zm {
namespace storeevent {

// The built-in default trigger types, used when the configured list is empty.
inline const std::array<std::string_view, 4>& default_trigger_types() {
    static const std::array<std::string_view, 4> kDefaults = {
        std::string_view("motion"),
        std::string_view("detection"),
        std::string_view("audio_event"),
        std::string_view("tracked_detection"),
    };
    return kDefaults;
}

// Returns true if `type` is a recognised trigger.
//
// - If `trigger_types` is non-empty: true iff `type` matches one of its entries.
// - If `trigger_types` is empty: true iff `type` matches one of the built-in
//   defaults (see default_trigger_types()).
// - An empty `type` never triggers.
inline bool is_trigger(const std::vector<std::string>& trigger_types,
                       const std::string& type) {
    if (type.empty())
        return false;

    if (trigger_types.empty()) {
        for (const auto& d : default_trigger_types()) {
            if (type == d)
                return true;
        }
        return false;
    }

    for (const auto& t : trigger_types) {
        if (t == type)
            return true;
    }
    return false;
}

}  // namespace storeevent
}  // namespace zm
