// privacy_mask: permanently obscure regions in DECODED frames.
//
// A PROCESS plugin for GDPR / privacy compliance: blur, pixelate, or black out
// regions so they are masked everywhere downstream — in recording, live
// streaming and any later analysis alike.
//
// Two kinds of region, usable together:
//   - static  `regions`: fixed polygons (windows, a neighbour's property, a
//             public footpath). Place AFTER decode and BEFORE detect/encode so
//             detection never sees them either.
//   - dynamic `dynamic`: whatever the detectors found on this frame — people,
//             faces, number plates — from "detection", "tracked_detection",
//             "face" and "lpr" events. Place DOWNSTREAM of those detectors
//             (and before encode/store/output): detectors publish their event
//             before forwarding the frame, so the boxes for frame N are already
//             here when frame N arrives. See dynamic_mask.hpp.
//
// It only touches uncompressed CPU frames (RGB24 / GRAYSCALE) matching the
// optional stream filter and the configured dimensions. Anything else
// (compressed packets, GPU surfaces, other streams, wrong size) is forwarded
// unchanged. It NEVER mutates the caller's buffer: the [hdr][payload] is copied
// into a local vector, masking is applied to the copy, and the copy is forwarded.
//
// LIFETIME of the dynamic state mirrors analytics_rules/tracker: it is a raw
// leaked pointer passed as the subscribe_evt `user`; stop() unsubscribes, then
// flips `running` so an in-flight callback no-ops, and never frees it.

#include "dynamic_mask.hpp"
#include "mask_util.hpp"

#include <zm_plugin.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>
#include <algorithm>
#include <cstdint>
#include <cstring>

using json = nlohmann::json;

namespace {

enum class MaskMode { Black, Blur, Pixelate };

constexpr uint64_t kStatsEvery = 500;

// Dynamic masking state, shared with the event callback. Leaked on stop.
struct DynamicState {
    std::atomic<bool> running{false};
    void* subHandle = nullptr;

    // Config (immutable after start).
    std::unordered_set<std::string> sources;   // event types to take boxes from
    std::unordered_set<std::string> classes;   // detection labels; empty = all
    float minConfidence = 0.25f;
    float padding = 0.15f;
    bool personHead = false;                   // mask only the top of person boxes
    float headFraction = 0.3f;

    std::mutex mutex;
    zm::privacy::BoxStore store;

    explicit DynamicState(uint64_t holdUsec) : store(holdUsec) {}
};

struct PrivacyMaskCtx {
    zm_host_api_t* host = nullptr;
    void* hostCtx = nullptr;

    // Config.
    int frameWidth = 0;
    int frameHeight = 0;
    MaskMode mode = MaskMode::Black;
    int blurSize = 16;
    std::vector<int> streamFilter;
    std::vector<std::vector<zm::privacy::Pt>> regions;
    DynamicState* dyn = nullptr;               // null = dynamic masking off

    // Stats (on_frame thread only).
    uint64_t framesSeen = 0, framesMasked = 0, boxesMasked = 0;
};

// Take the boxes this event contributes and store them for its (stream, pts).
void onDynamicEvent(DynamicState* st, const char* msg) {
    if (!st || !msg || !st->running.load()) return;
    json j;
    try { j = json::parse(msg); } catch (const std::exception&) { return; }
    if (!j.is_object()) return;
    const std::string type = j.value("type", std::string());
    if (!st->sources.count(type)) return;

    const char* listKey = (type == "face") ? "faces" : (type == "lpr") ? "plates" : "detections";
    if (!j.contains(listKey) || !j[listKey].is_array()) return;

    std::vector<zm::privacy::Box> boxes;
    for (const auto& d : j[listKey]) {
        if (!d.is_object() || !d.contains("bbox") || !d["bbox"].is_array() || d["bbox"].size() < 4)
            continue;
        const auto& b = d["bbox"];
        zm::privacy::Box box{b[0].get<float>(), b[1].get<float>(), b[2].get<float>(), b[3].get<float>()};
        if (type == "detection" || type == "tracked_detection") {
            // Faces and plates are always masked when their source is enabled: a
            // recogniser's similarity or an OCR confidence says nothing about
            // whether the region is identifying. Object detections are filtered.
            const std::string label = d.value("label", std::string());
            if (!st->classes.empty() && !st->classes.count(label)) continue;
            if (d.value("confidence", 1.0f) < st->minConfidence) continue;
            if (st->personHead && label == "person") box = zm::privacy::head_of(box, st->headFraction);
        }
        boxes.push_back(box);
    }
    if (boxes.empty()) return;

    std::lock_guard<std::mutex> lock(st->mutex);
    st->store.add(j.value("stream_id", 0), j.value("pts_usec", uint64_t{0}), boxes);
}

void forwardFrame(PrivacyMaskCtx* ctx, const void* buf, size_t size) {
    if (ctx && ctx->host && ctx->host->on_frame)
        ctx->host->on_frame(ctx->hostCtx, buf, size);
}

// Returns channel count for a maskable uncompressed format, or 0 otherwise.
int channels_for_hw_type(uint32_t hw_type) {
    switch (hw_type) {
        case ZM_FRAME_RGB24:     return 3;
        case ZM_FRAME_GRAYSCALE: return 1;
        default:                 return 0;  // compressed / GPU / yuv — skip
    }
}

int privacy_mask_start(zm_plugin_t* plugin, zm_host_api_t* host, void* host_ctx,
                       const char* json_cfg) {
    auto* ctx = new PrivacyMaskCtx();
    ctx->host = host;
    ctx->hostCtx = host_ctx;
    zm_plugin_set_log_context(host, host_ctx);
    try {
        auto j = json::parse(json_cfg ? json_cfg : "{}");
        ctx->frameWidth = j.value("frame_width", 0);
        ctx->frameHeight = j.value("frame_height", 0);
        ctx->blurSize = std::max(1, j.value("blur_size", 16));

        const std::string mode = j.value("mode", std::string("black"));
        if (mode == "blur") ctx->mode = MaskMode::Blur;
        else if (mode == "pixelate") ctx->mode = MaskMode::Pixelate;
        else ctx->mode = MaskMode::Black;

        if (j.contains("stream_filter") && j["stream_filter"].is_array())
            for (const auto& s : j["stream_filter"])
                ctx->streamFilter.push_back(s.get<int>());

        if (j.contains("regions") && j["regions"].is_array()) {
            for (const auto& poly : j["regions"]) {
                if (!poly.is_array()) continue;
                std::vector<zm::privacy::Pt> pts;
                for (const auto& pt : poly) {
                    if (pt.is_array() && pt.size() >= 2) {
                        pts.push_back({pt[0].get<float>(), pt[1].get<float>()});
                    }
                }
                if (pts.size() >= 3) ctx->regions.push_back(std::move(pts));
            }
        }

        if (j.contains("dynamic") && j["dynamic"].is_object() &&
            j["dynamic"].value("enabled", true)) {
            const auto& d = j["dynamic"];
            const double holdMs = std::max(0.0, d.value("hold_ms", 400.0));
            auto* st = new DynamicState(static_cast<uint64_t>(holdMs * 1000.0));
            std::vector<std::string> sources = {"detection", "tracked_detection", "face", "lpr"};
            if (d.contains("sources") && d["sources"].is_array())
                sources = d["sources"].get<std::vector<std::string>>();
            st->sources.insert(sources.begin(), sources.end());
            std::vector<std::string> classes = {"person"};
            if (d.contains("classes") && d["classes"].is_array())
                classes = d["classes"].get<std::vector<std::string>>();
            st->classes.insert(classes.begin(), classes.end());
            st->minConfidence = d.value("min_confidence", st->minConfidence);
            st->padding = std::max(0.0f, d.value("padding", st->padding));
            st->personHead = d.value("person_region", std::string("body")) == "head";
            st->headFraction = d.value("head_fraction", st->headFraction);
            ctx->dyn = st;
        }
    } catch (const std::exception& e) {
        ZM_LOG_ERROR("privacy_mask: failed to parse config: %s", e.what());
    }

    if (ctx->dyn && host && host->subscribe_evt) {
        ctx->dyn->running.store(true);
        ctx->dyn->subHandle = host->subscribe_evt(
            host_ctx,
            [](void* user, const char* evt) { onDynamicEvent(static_cast<DynamicState*>(user), evt); },
            ctx->dyn);
    }

    const char* modeName = ctx->mode == MaskMode::Blur ? "blur"
                          : ctx->mode == MaskMode::Pixelate ? "pixelate" : "black";
    ZM_LOG_INFO("privacy_mask: regions=%zu dynamic=%s mode=%s blur_size=%d dims=%dx%d",
                ctx->regions.size(), ctx->dyn ? "on" : "off", modeName, ctx->blurSize,
                ctx->frameWidth, ctx->frameHeight);
    if (ctx->dyn && (ctx->frameWidth <= 0 || ctx->frameHeight <= 0))
        ZM_LOG_WARN("privacy_mask: dynamic masking needs frame_width/frame_height; frames will pass unmasked");
    plugin->instance = ctx;
    return 0;
}

void logStats(PrivacyMaskCtx* ctx, const char* when) {
    if (!ctx->dyn) return;
    ZM_LOG_INFO("privacy_mask stats (%s): frames=%llu masked=%llu boxes=%llu", when,
                (unsigned long long)ctx->framesSeen, (unsigned long long)ctx->framesMasked,
                (unsigned long long)ctx->boxesMasked);
}

void privacy_mask_stop(zm_plugin_t* plugin) {
    if (!plugin || !plugin->instance) return;
    auto* ctx = static_cast<PrivacyMaskCtx*>(plugin->instance);
    if (ctx->dyn) {
        logStats(ctx, "final");
        if (ctx->host && ctx->host->unsubscribe_evt)
            ctx->host->unsubscribe_evt(ctx->hostCtx, ctx->dyn->subHandle);
        ctx->dyn->running.store(false);   // intentionally leaked, see top of file
    }
    delete ctx;
    plugin->instance = nullptr;
}

void privacy_mask_on_frame(zm_plugin_t* plugin, const void* buf, size_t size) {
    auto* ctx = static_cast<PrivacyMaskCtx*>(plugin->instance);
    if (!ctx || !buf || size < sizeof(zm_frame_hdr_t)) {
        forwardFrame(ctx, buf, size);
        return;
    }

    const auto* hdr = static_cast<const zm_frame_hdr_t*>(buf);

    const int channels = channels_for_hw_type(hdr->hw_type);
    if (channels == 0 || (ctx->regions.empty() && !ctx->dyn)) {
        forwardFrame(ctx, buf, size);  // not maskable / nothing to do
        return;
    }

    if (!ctx->streamFilter.empty() &&
        std::find(ctx->streamFilter.begin(), ctx->streamFilter.end(),
                  static_cast<int>(hdr->stream_id)) == ctx->streamFilter.end()) {
        forwardFrame(ctx, buf, size);
        return;
    }

    const int w = ctx->frameWidth, h = ctx->frameHeight;
    const size_t need = static_cast<size_t>(w) * h * channels;
    if (w <= 0 || h <= 0 || size < sizeof(zm_frame_hdr_t) + need) {
        forwardFrame(ctx, buf, size);  // can't safely address pixels; don't drop
        return;
    }

    // Boxes for this frame, if dynamic masking is on.
    std::vector<zm::privacy::Rect> rects;
    if (ctx->dyn) {
        ctx->framesSeen++;
        std::vector<zm::privacy::Box> boxes;
        {
            std::lock_guard<std::mutex> lock(ctx->dyn->mutex);
            boxes = ctx->dyn->store.lookup(static_cast<int>(hdr->stream_id), hdr->pts_usec);
        }
        for (const auto& b : boxes) {
            const auto r = zm::privacy::expand_clamp(b, ctx->dyn->padding, w, h);
            if (r.valid()) rects.push_back(r);
        }
        if (!rects.empty()) { ctx->framesMasked++; ctx->boxesMasked += rects.size(); }
        if (ctx->framesSeen % kStatsEvery == 0) logStats(ctx, "periodic");
    }
    if (ctx->regions.empty() && rects.empty()) {
        forwardFrame(ctx, buf, size);   // nothing to mask on this frame; no copy
        return;
    }

    // Copy [hdr][payload] so we never mutate the caller's buffer.
    std::vector<uint8_t> copy(static_cast<const uint8_t*>(buf),
                              static_cast<const uint8_t*>(buf) + size);
    uint8_t* px = copy.data() + sizeof(zm_frame_hdr_t);

    for (const auto& r : rects) {
        switch (ctx->mode) {
            case MaskMode::Black:    zm::privacy::black_rect(px, w, h, channels, r); break;
            case MaskMode::Pixelate: zm::privacy::pixelate_rect(px, w, h, channels, r, ctx->blurSize); break;
            case MaskMode::Blur:     zm::privacy::blur_rect(px, w, h, channels, r, ctx->blurSize); break;
        }
    }

    for (const auto& poly : ctx->regions) {
        switch (ctx->mode) {
            case MaskMode::Black:
                zm::privacy::black_region(px, w, h, channels, poly);
                break;
            case MaskMode::Pixelate:
                zm::privacy::pixelate_region(px, w, h, channels, poly, ctx->blurSize);
                break;
            case MaskMode::Blur:
                zm::privacy::blur_region(px, w, h, channels, poly, ctx->blurSize);
                break;
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
    plugin->start = privacy_mask_start;
    plugin->stop = privacy_mask_stop;
    plugin->on_frame = privacy_mask_on_frame;
}
