

#include <xsimd/xsimd.hpp>
#include <cstring>
#include <vector>
#include <string>
#include <algorithm>
#include <memory>
#include <chrono>
#include <zm_plugin.h>

struct Config {
    int threshold = 18;
    int min_pixels = 800;
    int width = 0, height = 0;
    int downscale = 1; // 1=orig, 2=half, 0=custom
    int out_w = 0, out_h = 0;
};

struct MotionBasicCtx {
    zm_host_api_t* host = nullptr;
    void* host_ctx = nullptr;
    Config cfg;
    alignas(64) std::vector<uint8_t> bg;
    bool bg_ready = false;
};

static void parse_config(const char* json, Config& cfg) {
    if (!json) return;
    std::string s(json);
    auto find_int = [&](const char* key, int& val) {
        auto p = s.find(key);
        if (p != std::string::npos) {
            auto c = s.find(':', p);
            if (c != std::string::npos) {
                val = std::clamp(std::atoi(s.c_str() + c + 1), 0, 255*255);
            }
        }
    };
    find_int("threshold", cfg.threshold);
    find_int("min_pixels", cfg.min_pixels);
    auto p = s.find("downscale");
    if (p != std::string::npos) {
        auto q = s.find('"', p+10);
        if (q != std::string::npos) {
            auto r = s.find('"', q+1);
            if (r != std::string::npos) {
                std::string v = s.substr(q+1, r-q-1);
                if (v == "orig") cfg.downscale = 1;
                else if (v == "half") cfg.downscale = 2;
                else {
                    auto x = v.find('x');
                    if (x != std::string::npos) {
                        cfg.out_w = std::atoi(v.c_str());
                        cfg.out_h = std::atoi(v.c_str()+x+1);
                        cfg.downscale = 0;
                    }
                }
            }
        }
    }
    cfg.threshold = std::clamp(cfg.threshold, 1, 255);
    cfg.min_pixels = std::max(cfg.min_pixels, 1);
}


static int motion_basic_start(zm_plugin_t* plugin, zm_host_api_t* host, void* host_ctx, const char* json_cfg) {
    auto* ctx = new MotionBasicCtx;
    ctx->host = host;
    ctx->host_ctx = host_ctx;
    parse_config(json_cfg, ctx->cfg);
    plugin->instance = ctx;
    return 0;
}

static void motion_basic_stop(zm_plugin_t* plugin) {
    auto* ctx = static_cast<MotionBasicCtx*>(plugin->instance);
    delete ctx;
    plugin->instance = nullptr;
}

static void motion_basic_on_frame(zm_plugin_t* plugin, const zm_frame_hdr_t* hdr, const void* payload) {
    auto* ctx = static_cast<MotionBasicCtx*>(plugin->instance);
    auto& cfg = ctx->cfg;
    auto* host = ctx->host;
    void* host_ctx = ctx->host_ctx;
    if (hdr->hw_type != 0) {
        if (host && host->log) host->log(host_ctx, ZM_LOG_WARN, "GPU frame ignored");
        return;
    }
    int w = hdr->stream_id; // Should be width, but zm_frame_hdr_t does not have width/height in your ABI
    int h = hdr->flags;     // Should be height, but using flags as a placeholder
    // TODO: Replace above with real width/height if available in your zm_frame_hdr_t
    if (w <= 0 || h <= 0) return;
    size_t y_size = w * h;
    if (ctx->bg.size() != y_size) {
        ctx->bg.assign(y_size, 0);
        ctx->bg_ready = false;
    }
    const uint8_t* Y = static_cast<const uint8_t*>(payload);
    std::vector<uint8_t> y_plane;
    if (cfg.downscale == 2) { // half
        int ow = w/2, oh = h/2;
        y_plane.resize(ow*oh);
        for (int y=0; y<oh; ++y) for (int x=0; x<ow; ++x) {
            int s = (y*2)*w + (x*2);
            int sum = Y[s] + Y[s+1] + Y[s+w] + Y[s+w+1];
            y_plane[y*ow+x] = sum/4;
        }
        w = ow; h = oh; y_size = w*h;
        if (ctx->bg.size() != y_size) ctx->bg.assign(y_size, 0);
        Y = y_plane.data();
    }
    using b = xsimd::batch<uint8_t>;
    constexpr auto VL = b::size;
    size_t i = 0, count = 0;
    const uint8_t* bg = ctx->bg.data();
    b thr_vec(cfg.threshold);
    for (; i+VL<=y_size; i+=VL) {
        b yv = b::load_unaligned(Y+i);
        b bgv = b::load_unaligned(bg+i);
        b diff = xsimd::abs(yv - bgv);
        auto mask = xsimd::gt(diff, thr_vec);
        count += xsimd::reduce_add(xsimd::select(mask, b(1), b(0)));
    }
    for (; i<y_size; ++i) {
        if (std::abs(int(Y[i])-int(bg[i])) > cfg.threshold) ++count;
    }
    uint8_t* bgw = ctx->bg.data();
    i = 0;
    for (; i+VL<=y_size; i+=VL) {
        b yv = b::load_unaligned(Y+i);
        b bgv = b::load_unaligned(bgw+i);
        b upd = (bgv*uint8_t(31) + yv)/uint8_t(32);
        upd.store_unaligned(bgw+i);
    }
    for (; i<y_size; ++i) {
        bgw[i] = (uint8_t)(((uint16_t)bgw[i]*31 + Y[i])/32);
    }
    if (count >= (size_t)cfg.min_pixels) {
        std::string msg = std::string("{\"mon\":") + std::to_string(hdr->stream_id) + ",\"pixels\":" + std::to_string(count) + "}";
        if (host && host->publish_evt) host->publish_evt(host_ctx, msg.c_str());
    }
}

#if defined(__GNUC__) || defined(__clang__)
#define ZM_PLUGIN_EXPORT __attribute__((visibility("default")))
#else
#define ZM_PLUGIN_EXPORT
#endif

extern "C" ZM_PLUGIN_EXPORT void zm_plugin_init(zm_plugin_t* plugin) {
    plugin->version = 1;
    plugin->type = ZM_PLUGIN_DETECT;
    plugin->start = motion_basic_start;
    plugin->stop = motion_basic_stop;
    plugin->on_frame = motion_basic_on_frame;
    plugin->instance = nullptr;
}
