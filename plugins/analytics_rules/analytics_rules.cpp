// analytics_rules: spatial analytics rules engine.
//
// Consumes "tracked_detection" events (from the tracker plugin) on the HOST
// event channel and raises high-value security alarms:
//   - intrusion : object enters a polygon zone (fires once per entry).
//   - linecross : object's path crosses a tripwire line (with direction).
//   - loiter    : object dwells inside a zone continuously for >= N seconds.
// and "pose" events (from detect_pose):
//   - fall      : a person goes from upright to down and stays down and still
//                 for >= N seconds. No model; see fall.hpp for the signals.
//
// INPUT events:
//   {"type":"tracked_detection","stream_id":N,"pts_usec":T,
//    "detections":[{"label","confidence","bbox":[x,y,w,h],"class_id",
//                   "track_id":K}, ...]}
//   {"type":"pose","stream_id":N,"pts_usec":T,
//    "persons":[{"confidence","bbox":[x,y,w,h],
//                "keypoints":[{"name","x","y","v"} x17 COCO order]}, ...]}
//
// Each object's GROUND POSITION is the bbox bottom-center (x+w/2, y+h) — the
// standard footfall-analytics anchor. We track the recent position and per-rule
// latch state per (stream_id, track_id).
//
// OUTPUT event (published via host->publish_evt on a fired rule):
//   {"type":"analytics","rule":"<name>","rule_type":"intrusion|linecross|loiter|fall",
//    "stream_id":N,"track_id":K,"label":"...","pts_usec":T,
//    ... rule-specific: "dwell_sec" (loiter) / "direction" (linecross) /
//    "down_sec","drift","tilt_deg","bbox" (fall)}
// For fall, track_id is the rule's own person id (pose events carry no tracker
// id), stable while that person stays in view.
//
// DESIGN: ZM_PLUGIN_PROCESS. on_frame is pure pass-through (frames forwarded
// untouched); all real work is event-driven via host->subscribe_evt.
//
// LIFETIME (mirrors plugins/tracker/tracker.cpp): the shared state struct is a
// raw leaked pointer passed as the `user` arg of subscribe_evt. stop()
// unsubscribes (so no future callbacks), then flips an atomic `running` flag so
// any already-in-flight callback no-ops; the state is intentionally NOT freed so
// an in-flight host callback never dereferences freed memory.

#include "fall.hpp"
#include "geometry.hpp"

#include <zm_plugin.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

using json = nlohmann::json;
using zm::analytics::Pt;

namespace {

// Prune tracks not seen for this long to bound memory.
constexpr uint64_t kStaleTrackUsec = 30ull * 1000000ull;  // 30 s

enum class RuleType { Intrusion, LineCross, Loiter, Fall };

// A single configured rule. Polygon/line vertices are pre-parsed to Pt.
struct Rule {
    std::string name;
    RuleType type = RuleType::Intrusion;

    std::vector<Pt> polygon;  // intrusion / loiter; optional for fall
    Pt line_a, line_b;        // linecross
    std::string direction = "any";  // linecross: "any" | "lr" | "rl"
    double seconds = 0.0;     // loiter
    zm::analytics::fall::Params fall;  // fall (fall.seconds is its own clock)

    bool has_stream = false;
    int stream_id = 0;        // when has_stream, only this stream is considered
    std::unordered_set<std::string> classes;  // empty => all classes
};

// Per-(track, rule) mutable state.
struct RuleTrackState {
    zm::analytics::ZoneState zone;  // intrusion / loiter latch
};

// Per object track, across all rules.
struct TrackState {
    bool has_prev = false;
    Pt prev;                 // previous ground position (for linecross)
    uint64_t last_seen = 0;  // pts of most recent sample (for pruning)
    // Per-rule latch keyed by rule index.
    std::map<std::size_t, RuleTrackState> rules;
};

// Shared, callback-owned state. Outlives the plugin instance on purpose.
struct AnalyticsState {
    zm_host_api_t* host = nullptr;
    void* hostCtx = nullptr;

    std::atomic<bool> running{false};
    void* subHandle = nullptr;

    std::vector<Rule> rules;

    // Per (stream_id, track_id) -> TrackState. Guarded by mutex (callback runs
    // on the publisher's thread).
    std::mutex mutex;
    std::map<std::pair<int, int>, TrackState> tracks;

    // Fall rules follow people themselves (pose events have no track id). Keyed
    // by (rule index, stream_id) so two fall rules with different thresholds
    // never share an episode latch.
    struct FallPeople {
        std::vector<zm::analytics::fall::Person> people;
        int next_id = 1;
    };
    std::map<std::pair<std::size_t, int>, FallPeople> fallPeople;
};

// Per-plugin-instance context. `state` is intentionally leaked on stop.
struct AnalyticsCtx {
    zm_host_api_t* host = nullptr;
    void* hostCtx = nullptr;
    AnalyticsState* state = nullptr;
};

bool parsePt(const json& a, Pt& out) {
    if (!a.is_array() || a.size() < 2) return false;
    out.x = a[0].get<float>();
    out.y = a[1].get<float>();
    return true;
}

bool parsePolygon(const json& arr, std::vector<Pt>& out) {
    if (!arr.is_array()) return false;
    for (const auto& v : arr) {
        Pt p;
        if (!parsePt(v, p)) return false;
        out.push_back(p);
    }
    return out.size() >= 3;
}

// Parse the "rules" config array into Rule structs. Tolerant: bad rules skipped.
void parseRules(const json& cfg, std::vector<Rule>& out) {
    if (!cfg.contains("rules") || !cfg["rules"].is_array()) return;
    for (const auto& rj : cfg["rules"]) {
        if (!rj.is_object()) continue;
        Rule r;
        r.name = rj.value("name", std::string("rule"));
        const std::string t = rj.value("type", std::string());

        if (t == "intrusion") {
            r.type = RuleType::Intrusion;
            if (!rj.contains("polygon") || !parsePolygon(rj["polygon"], r.polygon))
                continue;
        } else if (t == "linecross") {
            r.type = RuleType::LineCross;
            if (!rj.contains("line") || !rj["line"].is_array() ||
                rj["line"].size() < 2)
                continue;
            if (!parsePt(rj["line"][0], r.line_a) ||
                !parsePt(rj["line"][1], r.line_b))
                continue;
            r.direction = rj.value("direction", std::string("any"));
        } else if (t == "loiter") {
            r.type = RuleType::Loiter;
            if (!rj.contains("polygon") || !parsePolygon(rj["polygon"], r.polygon))
                continue;
            r.seconds = rj.value("seconds", 0.0);
        } else if (t == "fall") {
            r.type = RuleType::Fall;
            if (rj.contains("polygon") && !parsePolygon(rj["polygon"], r.polygon))
                continue;  // a polygon was given but is malformed
            auto& f = r.fall;
            f.seconds = rj.value("seconds", f.seconds);
            f.tilt_deg = rj.value("tilt_deg", f.tilt_deg);
            f.aspect = rj.value("aspect", f.aspect);
            f.min_signals = rj.value("min_signals", f.min_signals);
            f.kp_conf = rj.value("keypoint_conf", f.kp_conf);
            f.max_drift = rj.value("max_drift", f.max_drift);
            f.require_upright = rj.value("require_upright", f.require_upright);
            f.upright_window_sec = rj.value("upright_window_sec", f.upright_window_sec);
        } else {
            continue;  // unknown rule type
        }

        if (rj.contains("stream_id") && rj["stream_id"].is_number()) {
            r.has_stream = true;
            r.stream_id = rj["stream_id"].get<int>();
        }
        if (rj.contains("classes") && rj["classes"].is_array()) {
            for (const auto& c : rj["classes"])
                if (c.is_string()) r.classes.insert(c.get<std::string>());
        }
        out.push_back(std::move(r));
    }
}

bool ruleAppliesToClass(const Rule& r, const std::string& label) {
    if (r.classes.empty()) return true;
    return r.classes.count(label) > 0;
}

// Direction of a crossing: sign of cross(line_a, line_b, p) — which side p is
// on. We compare prev vs cur side. "lr" / "rl" honor the configured convention:
// "lr" => crossing left-to-right (prev on left side, cur on right side).
// Side sign: cross > 0 == left of directed line a->b, < 0 == right.
const char* crossingDirection(const Rule& r, Pt prev, Pt cur) {
    const float sp = zm::analytics::cross(r.line_a, r.line_b, prev);
    const float sc = zm::analytics::cross(r.line_a, r.line_b, cur);
    if (sp > 0 && sc < 0) return "lr";  // left -> right
    if (sp < 0 && sc > 0) return "rl";  // right -> left
    return "any";  // touched/collinear at an endpoint; unknown direction
}

void emitAlarm(AnalyticsState* state, const Rule& r, int streamId, int trackId,
               const std::string& label, uint64_t pts,
               const char* ruleTypeStr) {
    if (!state->host || !state->host->publish_evt) return;
    json out;
    out["type"] = "analytics";
    out["rule"] = r.name;
    out["rule_type"] = ruleTypeStr;
    out["stream_id"] = streamId;
    out["track_id"] = trackId;
    out["label"] = label;
    out["pts_usec"] = pts;
    state->host->publish_evt(state->hostCtx, out.dump().c_str());
}

void emitLoiter(AnalyticsState* state, const Rule& r, int streamId, int trackId,
                const std::string& label, uint64_t pts, double dwellSec) {
    if (!state->host || !state->host->publish_evt) return;
    json out;
    out["type"] = "analytics";
    out["rule"] = r.name;
    out["rule_type"] = "loiter";
    out["stream_id"] = streamId;
    out["track_id"] = trackId;
    out["label"] = label;
    out["pts_usec"] = pts;
    out["dwell_sec"] = dwellSec;
    state->host->publish_evt(state->hostCtx, out.dump().c_str());
}

void emitLineCross(AnalyticsState* state, const Rule& r, int streamId,
                   int trackId, const std::string& label, uint64_t pts,
                   const char* dir) {
    if (!state->host || !state->host->publish_evt) return;
    json out;
    out["type"] = "analytics";
    out["rule"] = r.name;
    out["rule_type"] = "linecross";
    out["stream_id"] = streamId;
    out["track_id"] = trackId;
    out["label"] = label;
    out["pts_usec"] = pts;
    out["direction"] = dir;
    state->host->publish_evt(state->hostCtx, out.dump().c_str());
}

void emitFall(AnalyticsState* state, const Rule& r, int streamId, int personId,
              uint64_t pts, const zm::analytics::fall::Fire& f,
              const zm::analytics::fall::Pose& p, float tiltDeg) {
    if (!state->host || !state->host->publish_evt) return;
    json out;
    out["type"] = "analytics";
    out["rule"] = r.name;
    out["rule_type"] = "fall";
    out["stream_id"] = streamId;
    out["track_id"] = personId;
    out["label"] = "person";
    out["pts_usec"] = pts;
    out["down_sec"] = f.down_sec;
    out["drift"] = f.drift;
    out["tilt_deg"] = tiltDeg;
    out["bbox"] = {p.x, p.y, p.w, p.h};
    state->host->publish_evt(state->hostCtx, out.dump().c_str());
}

// Parse one detect_pose person. Keypoints are taken in array order (COCO-17),
// which is what detect_pose emits unless keypoint_names reorders them.
bool parsePose(const json& pj, zm::analytics::fall::Pose& out) {
    if (!pj.is_object() || !pj.contains("bbox") || !pj["bbox"].is_array() ||
        pj["bbox"].size() < 4)
        return false;
    const auto& b = pj["bbox"];
    out.x = b[0].get<float>(); out.y = b[1].get<float>();
    out.w = b[2].get<float>(); out.h = b[3].get<float>();
    out.kpts.clear();
    if (pj.contains("keypoints") && pj["keypoints"].is_array()) {
        for (const auto& k : pj["keypoints"]) {
            zm::analytics::fall::Keypoint kp;
            if (k.is_object()) {
                kp.x = k.value("x", 0.0f); kp.y = k.value("y", 0.0f); kp.v = k.value("v", 0.0f);
            }
            out.kpts.push_back(kp);
        }
    }
    return true;
}

// Fall rules on a "pose" event. Caller holds the mutex.
void handlePose(AnalyticsState* state, const json& j) {
    const int streamId = j.value("stream_id", 0);
    const uint64_t pts = j.value("pts_usec", uint64_t{0});
    if (!j.contains("persons") || !j["persons"].is_array()) return;

    std::vector<zm::analytics::fall::Pose> all;
    for (const auto& pj : j["persons"]) {
        zm::analytics::fall::Pose p;
        if (parsePose(pj, p)) all.push_back(std::move(p));
    }

    for (std::size_t ri = 0; ri < state->rules.size(); ++ri) {
        const Rule& r = state->rules[ri];
        if (r.type != RuleType::Fall) continue;
        if (r.has_stream && r.stream_id != streamId) continue;

        // Only people whose feet are in the zone, when the rule has one.
        std::vector<zm::analytics::fall::Pose> poses;
        for (const auto& p : all) {
            if (r.polygon.empty() ||
                zm::analytics::point_in_polygon(
                    r.polygon, zm::analytics::bbox_ground(p.x, p.y, p.w, p.h)))
                poses.push_back(p);
        }

        auto& fp = state->fallPeople[{ri, streamId}];
        const auto idx = zm::analytics::fall::associate(fp.people, poses, pts, fp.next_id);
        for (std::size_t i = 0; i < poses.size(); ++i) {
            auto& person = fp.people[idx[i]];
            const auto fire = zm::analytics::fall::step(person, poses[i], pts, r.fall);
            if (fire.fire) {
                const float tilt = zm::analytics::fall::posture(poses[i], r.fall).tilt_deg;
                emitFall(state, r, streamId, person.id, pts, fire, poses[i], tilt);
            }
        }
    }
}

// Drop tracks not seen within kStaleTrackUsec of `now`. Caller holds the mutex.
void pruneStale(AnalyticsState* state, uint64_t now) {
    for (auto it = state->tracks.begin(); it != state->tracks.end();) {
        if (now >= it->second.last_seen &&
            now - it->second.last_seen > kStaleTrackUsec) {
            it = state->tracks.erase(it);
        } else {
            ++it;
        }
    }
}

// Event callback. Runs on the publisher's thread. Gated on state->running.
void handleEvent(AnalyticsState* state, const std::string& msg) {
    if (!state || !state->running.load()) return;

    json j;
    try {
        j = json::parse(msg);
    } catch (const std::exception&) {
        return;
    }
    if (!j.is_object()) return;
    const std::string type = j.value("type", std::string());
    if (type == "pose") {
        std::lock_guard<std::mutex> lock(state->mutex);
        handlePose(state, j);
        return;
    }
    if (type != "tracked_detection") return;

    const int streamId = j.value("stream_id", 0);
    const uint64_t pts = j.value("pts_usec", uint64_t{0});
    if (!j.contains("detections") || !j["detections"].is_array()) return;

    std::lock_guard<std::mutex> lock(state->mutex);
    pruneStale(state, pts);

    for (const auto& det : j["detections"]) {
        if (!det.is_object()) continue;
        const int trackId = det.value("track_id", 0);
        if (trackId == 0) continue;  // untracked detection; nothing to follow
        if (!det.contains("bbox") || !det["bbox"].is_array() ||
            det["bbox"].size() < 4)
            continue;

        const auto& b = det["bbox"];
        const Pt cur = zm::analytics::bbox_ground(
            b[0].get<float>(), b[1].get<float>(), b[2].get<float>(),
            b[3].get<float>());
        const std::string label = det.value("label", std::string());

        TrackState& ts = state->tracks[{streamId, trackId}];
        const bool hadPrev = ts.has_prev;
        const Pt prev = ts.prev;

        for (std::size_t ri = 0; ri < state->rules.size(); ++ri) {
            const Rule& r = state->rules[ri];
            if (r.has_stream && r.stream_id != streamId) continue;
            if (!ruleAppliesToClass(r, label)) continue;

            RuleTrackState& rts = ts.rules[ri];

            switch (r.type) {
                case RuleType::Fall:
                    break;  // driven by "pose" events, not tracked detections
                case RuleType::Intrusion: {
                    const bool inside =
                        zm::analytics::point_in_polygon(r.polygon, cur);
                    if (zm::analytics::intrusion_step(rts.zone, inside))
                        emitAlarm(state, r, streamId, trackId, label, pts,
                                  "intrusion");
                    break;
                }
                case RuleType::Loiter: {
                    const bool inside =
                        zm::analytics::point_in_polygon(r.polygon, cur);
                    const auto res = zm::analytics::loiter_step(
                        rts.zone, inside, pts, r.seconds);
                    if (res.fire)
                        emitLoiter(state, r, streamId, trackId, label, pts,
                                   res.dwell_sec);
                    break;
                }
                case RuleType::LineCross: {
                    if (!hadPrev) break;  // need a previous position
                    if (!zm::analytics::segments_intersect(prev, cur, r.line_a,
                                                           r.line_b))
                        break;
                    const char* dir = crossingDirection(r, prev, cur);
                    if (r.direction != "any" && std::string(dir) != r.direction)
                        break;  // wrong direction; ignore
                    emitLineCross(state, r, streamId, trackId, label, pts, dir);
                    break;
                }
            }
        }

        ts.prev = cur;
        ts.has_prev = true;
        ts.last_seen = pts;
    }
}

void forwardFrame(AnalyticsCtx* ctx, const void* buf, size_t size) {
    if (ctx && ctx->host && ctx->host->on_frame)
        ctx->host->on_frame(ctx->hostCtx, buf, size);
}

int analytics_start(zm_plugin_t* plugin, zm_host_api_t* host, void* host_ctx,
                    const char* json_cfg) {
    auto* ctx = new AnalyticsCtx();
    ctx->host = host;
    ctx->hostCtx = host_ctx;
    zm_plugin_set_log_context(host, host_ctx);

    auto* state = new AnalyticsState();  // leaked on stop (see AnalyticsCtx)
    state->host = host;
    state->hostCtx = host_ctx;

    try {
        auto cfg = json::parse(json_cfg ? json_cfg : "{}");
        parseRules(cfg, state->rules);
    } catch (const std::exception& e) {
        ZM_LOG_ERROR("analytics_rules: failed to parse config: %s", e.what());
    }

    state->running.store(true);
    ctx->state = state;
    plugin->instance = ctx;

    // Subscribe via the HOST so we reach the host's single event bus (a plugin's
    // own EventBus instance is not shared across the dlopen boundary). `state`
    // is the leaked-on-stop user pointer, keeping in-flight callbacks safe.
    if (host && host->subscribe_evt) {
        state->subHandle = host->subscribe_evt(
            host_ctx,
            [](void* user, const char* json_event) {
                handleEvent(static_cast<AnalyticsState*>(user),
                            json_event ? json_event : "");
            },
            state);
    }

    ZM_LOG_INFO("analytics_rules: %zu rule(s) loaded", state->rules.size());
    return 0;
}

void analytics_stop(zm_plugin_t* plugin) {
    if (!plugin || !plugin->instance) return;
    auto* ctx = static_cast<AnalyticsCtx*>(plugin->instance);
    // Unsubscribe via the host so no future callbacks fire, then flip running
    // off so any in-flight callback no-ops. `state` is intentionally leaked.
    if (ctx->state) {
        if (ctx->host && ctx->host->unsubscribe_evt)
            ctx->host->unsubscribe_evt(ctx->hostCtx, ctx->state->subHandle);
        ctx->state->running.store(false);
    }
    delete ctx;
    plugin->instance = nullptr;
}

void analytics_on_frame(zm_plugin_t* plugin, const void* buf, size_t size) {
    auto* ctx = static_cast<AnalyticsCtx*>(plugin ? plugin->instance : nullptr);
    forwardFrame(ctx, buf, size);  // pass-through; analytics is event-driven.
}

}  // namespace

extern "C" __attribute__((visibility("default"))) void zm_plugin_init(
    zm_plugin_t* plugin) {
    if (!plugin) return;
    plugin->version = 1;
    plugin->type = ZM_PLUGIN_PROCESS;
    plugin->instance = nullptr;
    plugin->start = analytics_start;
    plugin->stop = analytics_stop;
    plugin->on_frame = analytics_on_frame;
}
