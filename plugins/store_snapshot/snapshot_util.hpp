// snapshot_util.hpp — pure helper for store_snapshot.
//
// should_snapshot() decides whether an incoming event "type" should cause a JPEG
// snapshot to be written, applying BOTH a type filter AND a throttle window. It
// is intentionally free of any FFmpeg / ABI dependency so it can be unit-tested
// on its own.
//
// Semantics of an EMPTY trigger_types list: it falls back to the built-in
// DEFAULT trigger set ("detection", "motion", "face", "lpr", "analytics",
// "audio_event"). This mirrors the plugin's documented default config value, so
// a plugin configured with no "trigger_types" key behaves the same as one
// configured with the explicit default array. (To disable snapshots entirely,
// configure a list that contains no matching event types, e.g. ["none"].)

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace zm {
namespace storesnapshot {

// The built-in default trigger types, used when the configured list is empty.
inline const std::array<std::string_view, 6>& default_trigger_types() {
    static const std::array<std::string_view, 6> kDefaults = {
        std::string_view("detection"),
        std::string_view("motion"),
        std::string_view("face"),
        std::string_view("lpr"),
        std::string_view("analytics"),
        std::string_view("audio_event"),
    };
    return kDefaults;
}

// Returns true if `type` is a recognised trigger AND enough time has elapsed
// since the last snapshot.
//
// - Type filter:
//   - If `trigger_types` is non-empty: `type` must match one of its entries.
//   - If `trigger_types` is empty: `type` must match one of the built-in
//     defaults (see default_trigger_types()).
//   - An empty `type` never triggers.
// - Throttle: even for a matching type, returns false unless
//   (now_ms - last_ms) >= min_interval_ms. A last_ms of 0 (no prior snapshot)
//   and any non-positive min_interval_ms always pass the throttle gate.
inline bool should_snapshot(const std::vector<std::string>& trigger_types,
                            const std::string& type, int64_t now_ms,
                            int64_t last_ms, int64_t min_interval_ms) {
    if (type.empty())
        return false;

    bool type_ok = false;
    if (trigger_types.empty()) {
        for (const auto& d : default_trigger_types()) {
            if (type == d) {
                type_ok = true;
                break;
            }
        }
    } else {
        for (const auto& t : trigger_types) {
            if (t == type) {
                type_ok = true;
                break;
            }
        }
    }
    if (!type_ok)
        return false;

    if (min_interval_ms <= 0)
        return true;

    return (now_ms - last_ms) >= min_interval_ms;
}

}  // namespace storesnapshot
}  // namespace zm
