// alert_policy — collapse per-frame detections into per-OBJECT alerts.
//
// Subscribes to "tracked_detection" events (from the tracker plugin, which gives
// each object a persistent track_id) and emits an "alert" event ONCE when a new
// object appears, then stays quiet while it persists. So a parked car detected on
// every frame yields a single alert, not a stream of them. Optionally re-alerts
// when a long-stationary object starts moving again ("moving_again").
//
// PROCESS plugin; all work is event-driven (frames pass through untouched).
//
// Config:
//   classes          [labels]  filter which classes alert (empty = all)
//   stream_filter    [ids]     filter streams (empty = all)
//   move_frac        0.30      centroid shift / bbox-diagonal that counts as moving
//   stationary_sec   5.0       still this long => "stationary" (background)
//   realert_on_move  true      re-alert when a stationary object starts moving
//   cooldown_sec     0.0       min seconds between a track's alerts (0 = transitions only)

#include "zm_plugin.h"
#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

using json = nlohmann::json;

namespace {

struct Track {
    double cx = 0, cy = 0;
    uint64_t last_pts = 0;
    uint64_t last_alert_pts = 0;
    uint64_t still_since = 0;
    bool stationary = false;
    bool seen = false;
};

struct State {
    zm_host_api_t* host = nullptr;
    void* hostCtx = nullptr;
    std::vector<std::string> classes;     // empty = all
    std::vector<int> streamFilter;        // empty = all
    double moveFrac = 0.30;
    uint64_t stationaryUsec = 5'000'000;
    uint64_t cooldownUsec = 0;
    bool realertOnMove = true;
    std::unordered_map<uint64_t, Track> tracks;
    void* sub = nullptr;
    bool active = true;
};

bool allowedClass(const State* s, const std::string& l) {
    if (s->classes.empty()) return true;
    for (auto& c : s->classes) if (c == l) return true;
    return false;
}
bool allowedStream(const State* s, int sid) {
    if (s->streamFilter.empty()) return true;
    for (int v : s->streamFilter) if (v == sid) return true;
    return false;
}

void emitAlert(State* s, int sid, uint64_t pts, int tid, const std::string& label,
               const json& bbox, double conf, const char* reason) {
    if (!s->host || !s->host->publish_evt) return;
    json a;
    a["type"] = "alert"; a["stream_id"] = sid; a["pts_usec"] = pts;
    a["track_id"] = tid; a["label"] = label; a["bbox"] = bbox;
    a["confidence"] = conf; a["reason"] = reason;
    s->host->publish_evt(s->hostCtx, a.dump().c_str());
}

void handleEvent(State* s, const std::string& msg) {
    if (!s || !s->active) return;
    json j;
    try { j = json::parse(msg); } catch (...) { return; }
    if (j.value("type", std::string()) != "tracked_detection") return;
    const int sid = j.value("stream_id", 0);
    if (!allowedStream(s, sid)) return;
    const uint64_t pts = j.value("pts_usec", 0ull);
    if (!j.contains("detections") || !j["detections"].is_array()) return;

    for (const auto& d : j["detections"]) {
        const int tid = d.value("track_id", 0);
        if (tid <= 0) continue;                                  // unconfirmed track
        const std::string label = d.value("label", std::string());
        if (!allowedClass(s, label)) continue;
        if (!d.contains("bbox") || !d["bbox"].is_array() || d["bbox"].size() < 4) continue;
        const double x = d["bbox"][0], y = d["bbox"][1], w = d["bbox"][2], h = d["bbox"][3];
        const double cx = x + w / 2.0, cy = y + h / 2.0;
        const double diag = std::max(1.0, std::hypot(w, h));
        const double conf = d.value("confidence", 0.0);

        const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(sid)) << 32) |
                             static_cast<uint32_t>(tid);
        auto& t = s->tracks[key];
        const char* reason = nullptr;

        if (!t.seen) {                                           // brand-new object
            reason = "new";
            t.seen = true; t.still_since = pts;
        } else {
            const double moved = std::hypot(cx - t.cx, cy - t.cy);
            if (moved > s->moveFrac * diag) {                    // moving
                if (t.stationary && s->realertOnMove) reason = "moving_again";
                t.still_since = pts; t.stationary = false;
            } else {                                             // still
                if (t.still_since == 0) t.still_since = pts;
                if (pts - t.still_since >= s->stationaryUsec) t.stationary = true;
            }
        }

        if (reason) {
            const bool cooled = (t.last_alert_pts == 0) || (s->cooldownUsec == 0) ||
                                (pts - t.last_alert_pts >= s->cooldownUsec);
            if (std::string(reason) == "new" || cooled) {        // "new" always fires
                emitAlert(s, sid, pts, tid, label, d["bbox"], conf, reason);
                t.last_alert_pts = pts;
            }
        }
        t.cx = cx; t.cy = cy; t.last_pts = pts;
    }

    for (auto it = s->tracks.begin(); it != s->tracks.end();)    // prune tracks gone >30s
        if (pts > it->second.last_pts && pts - it->second.last_pts > 30'000'000ull)
            it = s->tracks.erase(it);
        else ++it;
}

int start(zm_plugin_t* plugin, zm_host_api_t* host, void* host_ctx, const char* json_cfg) {
    auto* s = new State();   // intentionally leaked on stop (in-flight callback safety)
    s->host = host; s->hostCtx = host_ctx;
    zm_plugin_set_log_context(host, host_ctx);
    try {
        auto cfg = json::parse(json_cfg ? json_cfg : "{}");
        if (cfg.contains("classes") && cfg["classes"].is_array())
            s->classes = cfg["classes"].get<std::vector<std::string>>();
        if (cfg.contains("stream_filter") && cfg["stream_filter"].is_array())
            s->streamFilter = cfg["stream_filter"].get<std::vector<int>>();
        s->moveFrac = cfg.value("move_frac", 0.30);
        s->stationaryUsec = static_cast<uint64_t>(cfg.value("stationary_sec", 5.0) * 1e6);
        s->cooldownUsec = static_cast<uint64_t>(cfg.value("cooldown_sec", 0.0) * 1e6);
        s->realertOnMove = cfg.value("realert_on_move", true);
    } catch (const std::exception& e) {
        ZM_LOG_ERROR("alert_policy: config parse failed: %s", e.what());
    }
    plugin->instance = s;
    if (host && host->subscribe_evt)
        s->sub = host->subscribe_evt(host_ctx,
            [](void* user, const char* js) { handleEvent(static_cast<State*>(user), js ? js : ""); }, s);
    ZM_LOG_INFO("alert_policy: started (one alert per track; realert_on_move=%d)", static_cast<int>(s->realertOnMove));
    return 0;
}

void stop(zm_plugin_t* plugin) {
    if (!plugin || !plugin->instance) return;
    auto* s = static_cast<State*>(plugin->instance);
    s->active = false;
    if (s->host && s->host->unsubscribe_evt && s->sub) s->host->unsubscribe_evt(s->hostCtx, s->sub);
    plugin->instance = nullptr;   // State leaked on purpose so a racing callback stays valid
}

void on_frame(zm_plugin_t* plugin, const void* buf, size_t size) {
    auto* s = plugin ? static_cast<State*>(plugin->instance) : nullptr;
    if (s && s->host && s->host->on_frame) s->host->on_frame(s->hostCtx, buf, size);  // pass-through
}

}  // namespace

extern "C" __attribute__((visibility("default"))) void zm_plugin_init(zm_plugin_t* plugin) {
    if (!plugin) return;
    plugin->version = 1;
    plugin->type = ZM_PLUGIN_PROCESS;
    plugin->instance = nullptr;
    plugin->start = start;
    plugin->stop = stop;
    plugin->on_frame = on_frame;
}
