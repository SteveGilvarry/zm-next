// motion_gate: a lightweight motion pre-filter for the AI cascade.
//
// Diffs downsampled luma between frames and, when "gate" is enabled, only forwards
// frames downstream while motion is active (plus a cooldown). Placed before an
// expensive stage (e.g. detect_onnx) it means YOLO only runs when something moves.
// It is a pass-through PROCESS plugin: GPU-surface frames and unknown formats are
// forwarded untouched (gating needs CPU luma).

#include "motion_diff.hpp"

#include <zm_plugin.h>
#include <nlohmann/json.hpp>

#include <string>
#include <vector>
#include <algorithm>
#include <cstdint>

using json = nlohmann::json;

namespace {

struct MotionGateCtx {
    zm_host_api_t* host = nullptr;
    void* hostCtx = nullptr;

    // Config.
    int frameWidth = 0;
    int frameHeight = 0;
    int step = 4;                 // downscale factor
    int pixelThreshold = 20;      // per-sample luma delta to count as "changed"
    int minChanged = 50;          // changed samples needed to declare motion
    int cooldownFrames = 15;      // keep the gate open this long after last motion
    bool gate = true;             // hard-gate downstream (drop static frames)
    std::vector<int> streamFilter;

    // State.
    std::vector<uint8_t> prev;
    int prevW = 0, prevH = 0;
    uint64_t frameCount = 0;
    uint64_t gateOpenUntil = 0;   // forward while frameCount <= this
};

void forwardFrame(MotionGateCtx* ctx, const void* buf, size_t size) {
    if (ctx && ctx->host && ctx->host->on_frame)
        ctx->host->on_frame(ctx->hostCtx, buf, size);
}

bool fmt_from_hw_type(uint32_t hw_type, zm::motiongate::PixFmt& out) {
    switch (hw_type) {
        case ZM_FRAME_RGB24:     out = zm::motiongate::PixFmt::RGB24;   return true;
        case ZM_FRAME_GRAYSCALE: out = zm::motiongate::PixFmt::GRAY8;   return true;
        case ZM_FRAME_YUV420P:   out = zm::motiongate::PixFmt::YUV420P; return true;
        default: return false;  // compressed or GPU surface — can't gate on CPU luma
    }
}

int motion_gate_start(zm_plugin_t* plugin, zm_host_api_t* host, void* host_ctx,
                      const char* json_cfg) {
    auto* ctx = new MotionGateCtx();
    ctx->host = host;
    ctx->hostCtx = host_ctx;
    zm_plugin_set_log_context(host, host_ctx);
    try {
        auto j = json::parse(json_cfg ? json_cfg : "{}");
        ctx->frameWidth = j.value("frame_width", 0);
        ctx->frameHeight = j.value("frame_height", 0);
        ctx->step = std::max(1, j.value("downscale", 4));
        ctx->pixelThreshold = j.value("pixel_threshold", 20);
        ctx->minChanged = j.value("min_changed_pixels", 50);
        ctx->cooldownFrames = j.value("cooldown_frames", 15);
        ctx->gate = j.value("gate", true);
        if (j.contains("stream_filter") && j["stream_filter"].is_array())
            for (const auto& s : j["stream_filter"]) ctx->streamFilter.push_back(s.get<int>());
    } catch (const std::exception& e) {
        ZM_LOG_ERROR("motion_gate: failed to parse config: %s", e.what());
    }
    ZM_LOG_INFO("motion_gate: gate=%d downscale=%d pixel_thr=%d min_changed=%d cooldown=%d",
                ctx->gate ? 1 : 0, ctx->step, ctx->pixelThreshold, ctx->minChanged,
                ctx->cooldownFrames);
    plugin->instance = ctx;
    return 0;
}

void motion_gate_stop(zm_plugin_t* plugin) {
    if (!plugin || !plugin->instance) return;
    delete static_cast<MotionGateCtx*>(plugin->instance);
    plugin->instance = nullptr;
}

void motion_gate_on_frame(zm_plugin_t* plugin, const void* buf, size_t size) {
    auto* ctx = static_cast<MotionGateCtx*>(plugin->instance);
    if (!ctx || !buf || size < sizeof(zm_frame_hdr_t)) { forwardFrame(ctx, buf, size); return; }

    const auto* hdr = static_cast<const zm_frame_hdr_t*>(buf);
    zm::motiongate::PixFmt fmt;
    if (!fmt_from_hw_type(hdr->hw_type, fmt)) { forwardFrame(ctx, buf, size); return; }

    if (!ctx->streamFilter.empty() &&
        std::find(ctx->streamFilter.begin(), ctx->streamFilter.end(),
                  static_cast<int>(hdr->stream_id)) == ctx->streamFilter.end()) {
        forwardFrame(ctx, buf, size);
        return;
    }

    const int w = ctx->frameWidth, h = ctx->frameHeight;
    const size_t need = (fmt == zm::motiongate::PixFmt::RGB24)
                            ? static_cast<size_t>(w) * h * 3
                            : static_cast<size_t>(w) * h;  // luma plane is enough
    if (w <= 0 || h <= 0 || size < sizeof(zm_frame_hdr_t) + need) {
        forwardFrame(ctx, buf, size);  // can't analyze; don't drop
        return;
    }

    const uint8_t* payload = static_cast<const uint8_t*>(buf) + sizeof(zm_frame_hdr_t);
    std::vector<uint8_t> cur;
    int dw = 0, dh = 0;
    zm::motiongate::downsample_luma(payload, fmt, w, h, ctx->step, cur, dw, dh);

    bool motion = false;
    if (ctx->prevW == dw && ctx->prevH == dh && !ctx->prev.empty()) {
        const int changed = zm::motiongate::count_changed(ctx->prev, cur, ctx->pixelThreshold);
        motion = (changed >= ctx->minChanged);
        if (motion) {
            ctx->gateOpenUntil = ctx->frameCount + static_cast<uint64_t>(ctx->cooldownFrames);
            if (ctx->host && ctx->host->publish_evt) {
                json evt;
                evt["type"] = "motion";
                evt["stream_id"] = hdr->stream_id;
                evt["changed"] = changed;
                evt["pts_usec"] = hdr->pts_usec;
                ctx->host->publish_evt(ctx->hostCtx, evt.dump().c_str());
            }
        }
    }
    ctx->prev = std::move(cur);
    ctx->prevW = dw;
    ctx->prevH = dh;

    const bool open = !ctx->gate || (ctx->frameCount <= ctx->gateOpenUntil);
    ++ctx->frameCount;
    if (open) forwardFrame(ctx, buf, size);
}

}  // namespace

extern "C" __attribute__((visibility("default"))) void zm_plugin_init(zm_plugin_t* plugin) {
    if (!plugin) return;
    plugin->version = 1;
    plugin->type = ZM_PLUGIN_PROCESS;
    plugin->instance = nullptr;
    plugin->start = motion_gate_start;
    plugin->stop = motion_gate_stop;
    plugin->on_frame = motion_gate_on_frame;
}
