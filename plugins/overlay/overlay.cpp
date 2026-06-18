// overlay: burn detection bounding boxes (and simple labels) onto decoded RGB24
// frames for human-viewable annotated output / recording.
//
// A PROCESS plugin. Detections do NOT arrive via on_frame — they arrive as host
// events (subscribe_evt). This plugin caches the MOST RECENT set of boxes per
// stream_id from events whose "type" is in the configured set (default:
// detection / tracked_detection / pose / face / lpr / segmentation). Each event's
// detections/persons/plates/objects array is parsed for a bbox [x,y,w,h] and an
// optional label (label / name / text / track_id).
//
// on_frame (RGB24 only, matching stream_filter, valid dims): COPY the
// [hdr][payload] into a local vector, draw each cached, non-expired box for this
// stream_id onto the copy's pixels (rectangle outline + optional label), and
// forward the copy via host->on_frame. Non-RGB24 / other streams / wrong size
// are forwarded unchanged. It NEVER mutates the caller's buffer.
//
// LIFETIME mirrors tracker.cpp: a raw, callback-owned OverlayState is used as the
// subscribe `user` and is intentionally LEAKED on stop (after unsubscribe) so an
// in-flight host callback never dereferences freed memory. `running` is atomic;
// all access to the per-stream box cache is mutex-guarded (the callback runs on
// the publisher's thread).

#include "draw.hpp"

#include <zm_plugin.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

using json = nlohmann::json;

namespace {

struct Box {
    int x = 0, y = 0, w = 0, h = 0;
    std::string label;
};

// Cached set of boxes for one stream, with the wall-clock time it was received
// (steady, milliseconds) so it can be expired by ttl_ms.
struct BoxSet {
    std::vector<Box> boxes;
    int64_t recvMs = 0;
};

int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// Shared, callback-owned state. Outlives the plugin instance on purpose.
struct OverlayState {
    zm_host_api_t* host = nullptr;
    void* hostCtx = nullptr;

    std::atomic<bool> running{false};
    void* subHandle = nullptr;

    // Config.
    std::vector<std::string> eventTypes;
    int thickness = 2;
    uint8_t r = 0, g = 255, b = 0;
    bool drawLabels = true;
    int labelScale = 1;
    int64_t ttlMs = 1000;
    int frameWidth = 0;
    int frameHeight = 0;
    std::vector<int> streamFilter;

    // Per stream_id cached boxes. Guarded by `mutex`.
    std::mutex mutex;
    std::map<int, BoxSet> cache;
};

// Per-plugin-instance context. `state` is leaked on stop (after unsubscribe).
struct OverlayCtx {
    zm_host_api_t* host = nullptr;
    void* hostCtx = nullptr;
    OverlayState* state = nullptr;
};

void forwardFrame(OverlayCtx* ctx, const void* buf, size_t size) {
    if (ctx && ctx->host && ctx->host->on_frame)
        ctx->host->on_frame(ctx->hostCtx, buf, size);
}

// Extract an optional label from a detection-like object.
std::string parseLabel(const json& det) {
    if (det.contains("label") && det["label"].is_string())
        return det["label"].get<std::string>();
    if (det.contains("name") && det["name"].is_string())
        return det["name"].get<std::string>();
    if (det.contains("text") && det["text"].is_string())
        return det["text"].get<std::string>();
    if (det.contains("track_id")) {
        if (det["track_id"].is_number_integer())
            return "ID " + std::to_string(det["track_id"].get<long long>());
        if (det["track_id"].is_string())
            return "ID " + det["track_id"].get<std::string>();
    }
    return std::string();
}

// Parse one detection-like object's bbox [x,y,w,h] + label into `out`.
bool parseBox(const json& det, Box& out) {
    if (!det.is_object()) return false;
    if (!det.contains("bbox") || !det["bbox"].is_array()) return false;
    const auto& bb = det["bbox"];
    if (bb.size() < 4) return false;
    out.x = static_cast<int>(bb[0].get<double>());
    out.y = static_cast<int>(bb[1].get<double>());
    out.w = static_cast<int>(bb[2].get<double>());
    out.h = static_cast<int>(bb[3].get<double>());
    out.label = parseLabel(det);
    return true;
}

// Collect boxes from any of the recognised detection-array keys.
void collectBoxes(const json& j, std::vector<Box>& out) {
    static const char* kArrays[] = {"detections", "persons", "plates",
                                    "objects", "faces", "segments"};
    for (const char* key : kArrays) {
        if (!j.contains(key) || !j[key].is_array()) continue;
        for (const auto& det : j[key]) {
            Box box;
            if (parseBox(det, box)) out.push_back(std::move(box));
        }
    }
}

// Event callback. Runs on the publisher's thread. Gated on state->running.
void handleEvent(OverlayState* state, const std::string& msg) {
    if (!state || !state->running.load()) return;

    json j;
    try {
        j = json::parse(msg);
    } catch (const std::exception&) {
        return;  // not JSON we understand
    }
    if (!j.is_object()) return;

    const std::string type = j.value("type", std::string());
    if (std::find(state->eventTypes.begin(), state->eventTypes.end(), type) ==
        state->eventTypes.end())
        return;  // not a type we overlay

    const int streamId = j.value("stream_id", 0);

    BoxSet set;
    collectBoxes(j, set.boxes);
    set.recvMs = now_ms();

    // Cache the MOST RECENT set per stream_id (replace, don't accumulate).
    std::lock_guard<std::mutex> lock(state->mutex);
    state->cache[streamId] = std::move(set);
}

int overlay_start(zm_plugin_t* plugin, zm_host_api_t* host, void* host_ctx,
                  const char* json_cfg) {
    auto* ctx = new OverlayCtx();
    ctx->host = host;
    ctx->hostCtx = host_ctx;
    zm_plugin_set_log_context(host, host_ctx);

    auto* state = new OverlayState();  // leaked on stop (see OverlayCtx)
    state->host = host;
    state->hostCtx = host_ctx;

    // Default event types.
    state->eventTypes = {"detection", "tracked_detection", "pose",
                         "face", "lpr", "segmentation"};

    try {
        auto cfg = json::parse(json_cfg ? json_cfg : "{}");

        if (cfg.contains("event_types") && cfg["event_types"].is_array()) {
            std::vector<std::string> types;
            for (const auto& t : cfg["event_types"])
                if (t.is_string()) types.push_back(t.get<std::string>());
            if (!types.empty()) state->eventTypes = std::move(types);
        }

        state->thickness = std::max(1, cfg.value("thickness", 2));
        state->drawLabels = cfg.value("draw_labels", true);
        state->labelScale = std::max(1, cfg.value("label_scale", 1));
        state->ttlMs = static_cast<int64_t>(cfg.value("ttl_ms", 1000));
        state->frameWidth = cfg.value("frame_width", 0);
        state->frameHeight = cfg.value("frame_height", 0);

        if (cfg.contains("color") && cfg["color"].is_array() &&
            cfg["color"].size() >= 3) {
            auto clamp8 = [](int v) {
                return static_cast<uint8_t>(std::max(0, std::min(255, v)));
            };
            state->r = clamp8(cfg["color"][0].get<int>());
            state->g = clamp8(cfg["color"][1].get<int>());
            state->b = clamp8(cfg["color"][2].get<int>());
        }

        if (cfg.contains("stream_filter") && cfg["stream_filter"].is_array())
            for (const auto& s : cfg["stream_filter"])
                state->streamFilter.push_back(s.get<int>());
    } catch (const std::exception& e) {
        ZM_LOG_ERROR("overlay: failed to parse config: %s", e.what());
    }

    state->running.store(true);
    ctx->state = state;
    plugin->instance = ctx;

    // Subscribe via the HOST so we reach the host's single event bus. `state` is
    // the user pointer; it is leaked on stop so an in-flight callback is safe.
    if (host && host->subscribe_evt) {
        state->subHandle = host->subscribe_evt(
            host_ctx,
            [](void* user, const char* json) {
                handleEvent(static_cast<OverlayState*>(user), json ? json : "");
            },
            state);
    }

    ZM_LOG_INFO("overlay: thickness=%d color=[%d,%d,%d] draw_labels=%d "
                "label_scale=%d ttl_ms=%lld dims=%dx%d event_types=%zu",
                state->thickness, state->r, state->g, state->b,
                state->drawLabels ? 1 : 0, state->labelScale,
                static_cast<long long>(state->ttlMs),
                state->frameWidth, state->frameHeight, state->eventTypes.size());
    return 0;
}

void overlay_stop(zm_plugin_t* plugin) {
    if (!plugin || !plugin->instance) return;
    auto* ctx = static_cast<OverlayCtx*>(plugin->instance);
    // Unsubscribe so no future callbacks fire, then flip running off so any
    // already-in-flight callback no-ops. `state` is intentionally leaked.
    if (ctx->state) {
        if (ctx->host && ctx->host->unsubscribe_evt)
            ctx->host->unsubscribe_evt(ctx->hostCtx, ctx->state->subHandle);
        ctx->state->running.store(false);
    }
    delete ctx;
    plugin->instance = nullptr;
}

void overlay_on_frame(zm_plugin_t* plugin, const void* buf, size_t size) {
    auto* ctx = static_cast<OverlayCtx*>(plugin ? plugin->instance : nullptr);
    OverlayState* state = ctx ? ctx->state : nullptr;

    if (!ctx || !state || !buf || size < sizeof(zm_frame_hdr_t)) {
        forwardFrame(ctx, buf, size);
        return;
    }

    const auto* hdr = static_cast<const zm_frame_hdr_t*>(buf);

    // Only RGB24 frames are annotated.
    if (hdr->hw_type != ZM_FRAME_RGB24) {
        forwardFrame(ctx, buf, size);
        return;
    }

    // Stream filter.
    if (!state->streamFilter.empty() &&
        std::find(state->streamFilter.begin(), state->streamFilter.end(),
                  static_cast<int>(hdr->stream_id)) == state->streamFilter.end()) {
        forwardFrame(ctx, buf, size);
        return;
    }

    const int w = state->frameWidth, h = state->frameHeight;
    const size_t need = static_cast<size_t>(w) * h * 3;
    if (w <= 0 || h <= 0 || size < sizeof(zm_frame_hdr_t) + need) {
        forwardFrame(ctx, buf, size);  // can't safely address pixels; don't drop
        return;
    }

    // Snapshot the cached, non-expired boxes for this stream.
    std::vector<Box> boxes;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        auto it = state->cache.find(static_cast<int>(hdr->stream_id));
        if (it != state->cache.end()) {
            const int64_t age = now_ms() - it->second.recvMs;
            if (state->ttlMs <= 0 || age <= state->ttlMs)
                boxes = it->second.boxes;  // copy out of the lock's reach
        }
    }

    if (boxes.empty()) {
        forwardFrame(ctx, buf, size);  // nothing to draw
        return;
    }

    // Copy [hdr][payload] so we never mutate the caller's buffer.
    std::vector<uint8_t> copy(static_cast<const uint8_t*>(buf),
                              static_cast<const uint8_t*>(buf) + size);
    uint8_t* px = copy.data() + sizeof(zm_frame_hdr_t);

    for (const auto& box : boxes) {
        zm::overlay::draw_rect(px, w, h, box.x, box.y, box.w, box.h,
                               state->r, state->g, state->b, state->thickness);
        if (state->drawLabels && !box.label.empty()) {
            // Draw the label just above the box; if it would clip off the top,
            // draw it just inside the top edge instead.
            int ty = box.y - (zm::overlay::kFontH * state->labelScale) - 1;
            if (ty < 0) ty = box.y + 1;
            zm::overlay::draw_text(px, w, h, box.x, ty, box.label,
                                   state->r, state->g, state->b,
                                   state->labelScale);
        }
    }

    forwardFrame(ctx, copy.data(), copy.size());
}

}  // namespace

extern "C" __attribute__((visibility("default"))) void zm_plugin_init(zm_plugin_t* plugin) {
    if (!plugin) return;
    plugin->version = 1;
    plugin->type = ZM_PLUGIN_PROCESS;
    plugin->instance = nullptr;
    plugin->start = overlay_start;
    plugin->stop = overlay_stop;
    plugin->on_frame = overlay_on_frame;
}
