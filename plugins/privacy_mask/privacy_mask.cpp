// privacy_mask: permanently obscure configured polygon regions in DECODED frames.
//
// A PROCESS plugin for GDPR / privacy compliance: blur, pixelate, or black out
// fixed regions (windows, a neighbour's property, a public footpath, etc.) so
// those areas are masked everywhere downstream — in detection, recording, and
// live streaming alike. Placed AFTER decode and BEFORE detect/encode.
//
// It only touches uncompressed CPU frames (RGB24 / GRAYSCALE) matching the
// optional stream filter and the configured dimensions. Anything else
// (compressed packets, GPU surfaces, other streams, wrong size) is forwarded
// unchanged. It NEVER mutates the caller's buffer: the [hdr][payload] is copied
// into a local vector, masking is applied to the copy, and the copy is forwarded.

#include "mask_util.hpp"

#include <zm_plugin.h>
#include <nlohmann/json.hpp>

#include <string>
#include <vector>
#include <algorithm>
#include <cstdint>
#include <cstring>

using json = nlohmann::json;

namespace {

enum class MaskMode { Black, Blur, Pixelate };

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
};

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
    } catch (const std::exception& e) {
        ZM_LOG_ERROR("privacy_mask: failed to parse config: %s", e.what());
    }

    const char* modeName = ctx->mode == MaskMode::Blur ? "blur"
                          : ctx->mode == MaskMode::Pixelate ? "pixelate" : "black";
    ZM_LOG_INFO("privacy_mask: regions=%zu mode=%s blur_size=%d dims=%dx%d",
                ctx->regions.size(), modeName, ctx->blurSize,
                ctx->frameWidth, ctx->frameHeight);
    plugin->instance = ctx;
    return 0;
}

void privacy_mask_stop(zm_plugin_t* plugin) {
    if (!plugin || !plugin->instance) return;
    delete static_cast<PrivacyMaskCtx*>(plugin->instance);
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
    if (channels == 0 || ctx->regions.empty()) {
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

    // Copy [hdr][payload] so we never mutate the caller's buffer.
    std::vector<uint8_t> copy(static_cast<const uint8_t*>(buf),
                              static_cast<const uint8_t*>(buf) + size);
    uint8_t* px = copy.data() + sizeof(zm_frame_hdr_t);

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
