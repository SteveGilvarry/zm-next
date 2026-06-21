// tracker: a multi-object tracker that assigns persistent track_ids to
// detections across frames (SORT/ByteTrack-style IoU association).
//
// Detections do NOT arrive via on_frame — they arrive as EventBus "plugin_event"
// messages published by detect_onnx:
//   {"type":"detection","stream_id":N,"pts_usec":T,
//    "detections":[{"label","confidence","bbox":[x,y,w,h],"class_id"}, ...]}
//
// This plugin subscribes to the EventBus, runs IoU-based association per
// stream_id, and republishes an enriched event via the host API (so it flows
// like every other event). The republished type is "tracked_detection" (not
// "detection") so it never re-enters our own subscriber and loops:
//   {"type":"tracked_detection","stream_id":N,"pts_usec":T,
//    "detections":[{...original..., "track_id":K}, ...]}
//
// It is a pass-through PROCESS plugin: on_frame just forwards frames untouched —
// frames are irrelevant to tracking here.
//
// LIFETIME: EventBus has no unsubscribe, so the subscribed callback can fire
// after stop(). The tracker state lives in a std::shared_ptr<TrackerState>
// captured by the lambda; stop() flips state->running to false but does NOT free
// the state (the shared_ptr keeps it alive — a small one-time leak is accepted,
// as documented in the build instructions). All access to the per-stream tracker
// map is guarded by a mutex because the callback runs on the publisher's thread.

#include "tracker_core.hpp"

#include <zm_plugin.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>

using json = nlohmann::json;

namespace {

// Shared, callback-owned state. Outlives the plugin instance on purpose.
struct TrackerState {
    zm_host_api_t* host = nullptr;
    void* hostCtx = nullptr;

    std::atomic<bool> running{false};
    void* subHandle = nullptr;

    // Config.
    float iouThreshold = 0.3f;
    int maxAge = 30;
    int minHits = 3;
    bool classGated = true;

    // Per stream_id tracker. Guarded by `mutex`.
    std::mutex mutex;
    std::map<int, zm::tracker::Tracker> trackers;

    zm::tracker::Tracker& trackerFor(int streamId) {
        auto it = trackers.find(streamId);
        if (it == trackers.end()) {
            it = trackers.emplace(
                       streamId, zm::tracker::Tracker(iouThreshold, maxAge,
                                                      minHits, classGated))
                     .first;
        }
        return it->second;
    }
};

// Per-plugin-instance context. `state` is intentionally leaked on stop (after
// unsubscribe) so an in-flight host callback never touches freed memory.
struct TrackerCtx {
    zm_host_api_t* host = nullptr;
    void* hostCtx = nullptr;
    TrackerState* state = nullptr;
};

// Parse [x,y,w,h] bbox out of a detection object; tolerant of missing/short arrays.
bool parseBbox(const json& det, zm::tracker::Det& out) {
    if (!det.contains("bbox") || !det["bbox"].is_array()) return false;
    const auto& b = det["bbox"];
    if (b.size() < 4) return false;
    out.x = b[0].get<float>();
    out.y = b[1].get<float>();
    out.w = b[2].get<float>();
    out.h = b[3].get<float>();
    out.class_id = det.value("class_id", -1);
    out.confidence = det.value("confidence", 0.0f);
    return true;
}

// Event callback. Runs on the publisher's thread. Gated on state->running.
void handleEvent(TrackerState* state, const std::string& msg) {
    if (!state || !state->running.load()) return;

    json j;
    try {
        j = json::parse(msg);
    } catch (const std::exception&) {
        return;  // not JSON we understand
    }
    if (!j.is_object() || j.value("type", std::string()) != "detection") return;

    const int streamId = j.value("stream_id", 0);
    if (!j.contains("detections") || !j["detections"].is_array()) return;

    // Build parallel det list, remembering which JSON entries were valid.
    const json& detsJson = j["detections"];
    std::vector<zm::tracker::Det> dets;
    std::vector<int> jsonIndex;  // dets[i] came from detsJson[jsonIndex[i]]
    dets.reserve(detsJson.size());
    jsonIndex.reserve(detsJson.size());
    for (std::size_t i = 0; i < detsJson.size(); ++i) {
        zm::tracker::Det d;
        if (parseBbox(detsJson[i], d)) {
            dets.push_back(d);
            jsonIndex.push_back(static_cast<int>(i));
        }
    }

    std::vector<int> ids;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        ids = state->trackerFor(streamId).update(dets);
    }

    // Build enriched event: copy originals, attach track_id (0 == no track).
    json out;
    out["type"] = "tracked_detection";
    out["stream_id"] = streamId;
    if (j.contains("pts_usec")) out["pts_usec"] = j["pts_usec"];
    json outDets = json::array();
    // Map track_id back onto valid dets; pass through any unparsed entries with id 0.
    std::vector<int> idForJson(detsJson.size(), 0);
    for (std::size_t i = 0; i < ids.size(); ++i)
        idForJson[static_cast<std::size_t>(jsonIndex[i])] = ids[i];
    for (std::size_t i = 0; i < detsJson.size(); ++i) {
        json d = detsJson[i];
        d["track_id"] = idForJson[i];
        outDets.push_back(std::move(d));
    }
    out["detections"] = std::move(outDets);

    // Publish via the host API (NOT EventBus directly) so it flows like other events.
    if (state->host && state->host->publish_evt)
        state->host->publish_evt(state->hostCtx, out.dump().c_str());
}

void forwardFrame(TrackerCtx* ctx, const void* buf, size_t size) {
    if (ctx && ctx->host && ctx->host->on_frame)
        ctx->host->on_frame(ctx->hostCtx, buf, size);
}

int tracker_start(zm_plugin_t* plugin, zm_host_api_t* host, void* host_ctx,
                  const char* json_cfg) {
    auto* ctx = new TrackerCtx();
    ctx->host = host;
    ctx->hostCtx = host_ctx;
    zm_plugin_set_log_context(host, host_ctx);

    auto* state = new TrackerState();  // leaked on stop (see TrackerCtx)
    state->host = host;
    state->hostCtx = host_ctx;

    try {
        auto cfg = json::parse(json_cfg ? json_cfg : "{}");
        state->iouThreshold = cfg.value("iou_threshold", 0.3f);
        state->maxAge = cfg.value("max_age", 30);
        state->minHits = cfg.value("min_hits", 3);
        state->classGated = cfg.value("class_gated", true);
    } catch (const std::exception& e) {
        ZM_LOG_ERROR("tracker: failed to parse config: %s", e.what());
    }

    state->running.store(true);
    ctx->state = state;
    plugin->instance = ctx;

    // Subscribe via the HOST so we reach the host's single event bus (a plugin's
    // own EventBus instance is not shared across the dlopen boundary). `state` is
    // the user pointer; it is leaked on stop so an in-flight callback is safe.
    if (host && host->subscribe_evt) {
        state->subHandle = host->subscribe_evt(
            host_ctx,
            [](void* user, const char* json) {
                handleEvent(static_cast<TrackerState*>(user), json ? json : "");
            },
            state);
    }

    ZM_LOG_INFO("tracker: iou_threshold=%.2f max_age=%d min_hits=%d class_gated=%d",
                state->iouThreshold, state->maxAge, state->minHits,
                state->classGated);
    return 0;
}

void tracker_stop(zm_plugin_t* plugin) {
    if (!plugin || !plugin->instance) return;
    auto* ctx = static_cast<TrackerCtx*>(plugin->instance);
    // Unsubscribe via the host so no future callbacks fire, then flip running off
    // so any already-in-flight callback no-ops. `state` is intentionally leaked
    // (not deleted) so an in-flight callback never dereferences freed memory.
    if (ctx->state) {
        if (ctx->host && ctx->host->unsubscribe_evt)
            ctx->host->unsubscribe_evt(ctx->hostCtx, ctx->state->subHandle);
        ctx->state->running.store(false);
    }
    delete ctx;
    plugin->instance = nullptr;
}

void tracker_on_frame(zm_plugin_t* plugin, const void* buf, size_t size) {
    auto* ctx = static_cast<TrackerCtx*>(plugin ? plugin->instance : nullptr);
    forwardFrame(ctx, buf, size);  // pass-through; tracking is event-driven.
}

}  // namespace

extern "C" __attribute__((visibility("default"))) void zm_plugin_init(zm_plugin_t* plugin) {
    if (!plugin) return;
    plugin->version = 1;
    plugin->type = ZM_PLUGIN_PROCESS;
    plugin->instance = nullptr;
    plugin->start = tracker_start;
    plugin->stop = tracker_stop;
    plugin->on_frame = tracker_on_frame;
}
