// store_snapshot: write a JPEG snapshot/thumbnail to disk when an alarm event
// fires — for event thumbnails in the UI.
//
// This is a ZM_PLUGIN_PROCESS pass-through. It keeps the latest decoded RGB24
// frame in memory (under a mutex) and, when a triggering event arrives via the
// host event subscription, JPEG-encodes that frame and writes it to:
//
//   {root}/{YYYY-MM-DD}/snap-{stream}-{HH-MM-SS-mmm}.jpg
//
// Snapshots are throttled to at most one per `min_interval_ms` so a burst of
// detections does not flood the disk.
//
// Data flow:
//   on_frame (capture thread):
//     - if the frame is ZM_FRAME_RGB24 and passes stream_filter, copy it into
//       the "latest frame" buffer under the mutex,
//     - ALWAYS forward downstream via host->on_frame (pass-through). Non-RGB24
//       frames are forwarded untouched.
//   event callback (publisher thread, via host->subscribe_evt):
//     - if the event "type" is a configured trigger and the throttle window has
//       elapsed, snapshot the latest frame under the mutex, JPEG-encode it, and
//       write it to disk.
//
// Lifetime: state is a raw, leaked struct handed to the host callback as `user`
// plus an atomic `running`; we unsubscribe in stop(). The event callback (reader
// of the latest frame) and on_frame (writer) are serialised by st->mtx.

#include "snapshot_util.hpp"

#include <zm_plugin.h>
#include <nlohmann/json.hpp>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

// ---------------------------------------------------------------------------
// Plugin state. Leaked on stop() so an in-flight host callback never dangles.
// ---------------------------------------------------------------------------
struct StoreSnapshotState {
    // Config.
    std::string root;
    std::vector<std::string> trigger_types;   // empty == built-in defaults
    int64_t min_interval_ms = 2000;
    // FFmpeg mjpeg quality on the native qscale (2..31, lower == better).
    // Default 5. Clamped into [2,31] at start.
    int jpeg_quality = 5;
    int frame_width = 0;                       // optional override; else derived
    int frame_height = 0;
    std::vector<uint32_t> stream_filter;       // empty == accept all

    // Host.
    zm_host_api_t* host = nullptr;
    void* host_ctx = nullptr;
    void* sub_handle = nullptr;

    // Lifetime gate for the host callback.
    std::atomic<bool> running{false};

    // Latest RGB24 frame + throttle state — guarded by mtx. on_frame (capture
    // thread) writes the frame; the event callback (publisher thread) reads it.
    std::mutex mtx;
    std::vector<uint8_t> latest_frame;
    int latest_width = 0;
    int latest_height = 0;
    uint32_t latest_stream_id = 0;
    int64_t latest_pts_usec = 0;
    bool have_frame = false;
    int64_t last_snapshot_ms = 0;             // monotonic-ish wall clock (ms)
    bool warned_no_frame = false;             // log-once when triggered too early
};

// Couples plugin->instance to the (leaked) state.
struct StoreSnapshotCtx {
    zm_host_api_t* host = nullptr;
    void* host_ctx = nullptr;
    StoreSnapshotState* state = nullptr;
};

// ---------------------------------------------------------------------------
// Logging.
// ---------------------------------------------------------------------------
void slog(StoreSnapshotState* st, zm_log_level_t level, const char* fmt, ...) {
    if (!st || !st->host || !st->host->log) return;
    va_list ap;
    va_start(ap, fmt);
    char buf[512];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    st->host->log(st->host_ctx, level, buf);
}

std::string get_default_root() {
#ifdef __APPLE__
    return "/Shared/zm/media";
#elif defined(_WIN32)
    return "C:/ZM/media";
#else
    return "/lib/zm/media";
#endif
}

int64_t now_wall_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// "{root}/{YYYY-MM-DD}/snap-{stream}-{HH-MM-SS-mmm}.jpg"
std::string make_snapshot_path(const std::string& root, uint32_t stream_id) {
    int64_t ms = now_wall_ms();
    std::time_t t = (std::time_t)(ms / 1000);
    int millis = (int)(ms % 1000);
    std::tm tm = *std::localtime(&t);
    char dir[512];
    std::snprintf(dir, sizeof(dir), "%s/%04d-%02d-%02d", root.c_str(),
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    char name[256];
    std::snprintf(name, sizeof(name), "snap-%u-%02d-%02d-%02d-%03d.jpg",
                  stream_id, tm.tm_hour, tm.tm_min, tm.tm_sec, millis);
    return std::string(dir) + "/" + std::string(name);
}

bool stream_allowed(const StoreSnapshotState* st, uint32_t sid) {
    if (st->stream_filter.empty()) return true;
    return std::find(st->stream_filter.begin(), st->stream_filter.end(), sid) !=
           st->stream_filter.end();
}

// ---------------------------------------------------------------------------
// JPEG encoding helper (FFmpeg mjpeg encoder). Mirrors describe_vlm: sws_scale
// RGB24 -> YUVJ420P, then the mjpeg encoder. `quality` is the native FFmpeg
// qscale (2..31, lower == better quality / larger file).
// ---------------------------------------------------------------------------
bool encode_rgb24_to_jpeg(const uint8_t* rgb, int width, int height,
                          int quality, std::vector<uint8_t>& out) {
    out.clear();
    if (!rgb || width <= 0 || height <= 0) return false;
    if (quality < 2) quality = 2;
    if (quality > 31) quality = 31;

    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    if (!codec) return false;

    AVCodecContext* cctx = avcodec_alloc_context3(codec);
    if (!cctx) return false;

    cctx->pix_fmt = AV_PIX_FMT_YUVJ420P;
    cctx->width = width;
    cctx->height = height;
    cctx->time_base = AVRational{1, 25};
    cctx->color_range = AVCOL_RANGE_JPEG;
    // Drive mjpeg quality via fixed qscale (FF_QP2LAMBDA-scaled).
    cctx->flags |= AV_CODEC_FLAG_QSCALE;
    cctx->global_quality = quality * FF_QP2LAMBDA;

    SwsContext* sws = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* pkt = nullptr;
    bool ok = false;

    do {
        if (avcodec_open2(cctx, codec, nullptr) < 0) break;

        frame = av_frame_alloc();
        if (!frame) break;
        frame->format = cctx->pix_fmt;
        frame->width = width;
        frame->height = height;
        if (av_frame_get_buffer(frame, 32) < 0) break;

        sws = sws_getContext(width, height, AV_PIX_FMT_RGB24, width, height,
                             AV_PIX_FMT_YUVJ420P, SWS_BILINEAR, nullptr, nullptr,
                             nullptr);
        if (!sws) break;

        const uint8_t* srcSlice[1] = {rgb};
        int srcStride[1] = {3 * width};
        sws_scale(sws, srcSlice, srcStride, 0, height, frame->data,
                  frame->linesize);

        frame->pts = 0;
        frame->quality = cctx->global_quality;

        if (avcodec_send_frame(cctx, frame) < 0) break;

        pkt = av_packet_alloc();
        if (!pkt) break;

        int ret = avcodec_receive_packet(cctx, pkt);
        if (ret < 0) break;

        out.assign(pkt->data, pkt->data + pkt->size);
        ok = true;
    } while (false);

    if (pkt) av_packet_free(&pkt);
    if (frame) av_frame_free(&frame);
    if (sws) sws_freeContext(sws);
    if (cctx) avcodec_free_context(&cctx);

    return ok;
}

// Write a buffer to a file atomically-ish (parent dirs created). Returns true on
// success.
bool write_file(const std::string& path, const std::vector<uint8_t>& data) {
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    size_t n = data.empty() ? 0
                            : std::fwrite(data.data(), 1, data.size(), f);
    std::fclose(f);
    return n == data.size();
}

// ---------------------------------------------------------------------------
// Snapshot — invoked from the host event callback. Caller does NOT hold mtx;
// we take it here to copy the latest frame, then encode/IO outside the lock.
// ---------------------------------------------------------------------------
void take_snapshot(StoreSnapshotState* st, int64_t now_ms) {
    std::vector<uint8_t> rgb;
    int width = 0, height = 0;
    uint32_t stream_id = 0;
    {
        std::lock_guard<std::mutex> lk(st->mtx);
        if (!st->have_frame || st->latest_frame.empty()) {
            if (!st->warned_no_frame) {
                slog(st, ZM_LOG_WARN,
                     "store_snapshot: trigger before any RGB24 frame seen; "
                     "skipping snapshot");
                st->warned_no_frame = true;
            }
            return;
        }
        rgb = st->latest_frame;  // copy under lock
        width = st->latest_width;
        height = st->latest_height;
        stream_id = st->latest_stream_id;
        st->last_snapshot_ms = now_ms;  // reserve the throttle slot
    }

    if (width <= 0 || height <= 0) {
        slog(st, ZM_LOG_ERROR,
             "store_snapshot: invalid frame dimensions %dx%d (set frame_width/"
             "frame_height in config)",
             width, height);
        return;
    }
    if (rgb.size() < (size_t)width * (size_t)height * 3) {
        slog(st, ZM_LOG_ERROR,
             "store_snapshot: frame buffer too small for %dx%d RGB24", width,
             height);
        return;
    }

    std::vector<uint8_t> jpeg;
    if (!encode_rgb24_to_jpeg(rgb.data(), width, height, st->jpeg_quality,
                              jpeg)) {
        slog(st, ZM_LOG_ERROR, "store_snapshot: JPEG encode failed");
        return;
    }

    std::string path = make_snapshot_path(st->root, stream_id);
    if (!write_file(path, jpeg)) {
        slog(st, ZM_LOG_ERROR, "store_snapshot: failed to write %s",
             path.c_str());
        return;
    }

    slog(st, ZM_LOG_INFO, "store_snapshot: wrote %s (%zu bytes)", path.c_str(),
         jpeg.size());

    if (st->host && st->host->publish_evt) {
        json ev = {{"event", "EventSnapshot"},
                   {"path", path},
                   {"stream_id", stream_id}};
        st->host->publish_evt(st->host_ctx, ev.dump().c_str());
    }
}

// ---------------------------------------------------------------------------
// Event callback — publisher thread.
// ---------------------------------------------------------------------------
void event_cb(void* user, const char* json_event) {
    auto* st = static_cast<StoreSnapshotState*>(user);
    if (!st || !st->running.load(std::memory_order_acquire) || !json_event)
        return;

    json j;
    try {
        j = json::parse(json_event);
    } catch (...) {
        return;
    }

    std::string type;
    if (j.contains("type") && j["type"].is_string())
        type = j["type"].get<std::string>();
    if (type.empty()) return;

    uint32_t sid = j.value("stream_id", 0u);
    if (!stream_allowed(st, sid)) return;

    // Throttle check uses last_snapshot_ms under the mutex.
    int64_t now_ms = now_wall_ms();
    int64_t last_ms;
    {
        std::lock_guard<std::mutex> lk(st->mtx);
        last_ms = st->last_snapshot_ms;
    }
    if (!zm::storesnapshot::should_snapshot(st->trigger_types, type, now_ms,
                                            last_ms, st->min_interval_ms))
        return;

    take_snapshot(st, now_ms);
}

// ---------------------------------------------------------------------------
// Frame handling — capture thread.
// ---------------------------------------------------------------------------
void forward_downstream(StoreSnapshotState* st, const void* buf, size_t size) {
    if (st->host && st->host->on_frame)
        st->host->on_frame(st->host_ctx, buf, size);
}

void handle_frame(zm_plugin_t* plugin, const void* buf, size_t size) {
    auto* ctx = static_cast<StoreSnapshotCtx*>(plugin->instance);
    if (!ctx || !ctx->state || !buf) return;
    StoreSnapshotState* st = ctx->state;

    if (size < sizeof(zm_frame_hdr_t)) {
        forward_downstream(st, buf, size);
        return;
    }

    const zm_frame_hdr_t* hdr = static_cast<const zm_frame_hdr_t*>(buf);
    const uint8_t* payload =
        static_cast<const uint8_t*>(buf) + sizeof(zm_frame_hdr_t);
    size_t payload_size = size - sizeof(zm_frame_hdr_t);

    // Only RGB24 frames are snapshot-able; everything else just forwards.
    if (hdr->hw_type == (uint32_t)ZM_FRAME_RGB24 &&
        stream_allowed(st, hdr->stream_id) && payload_size > 0) {
        // Determine dimensions: prefer configured values; else derive assuming
        // RGB24 only works if we know one dimension, so configured values are
        // strongly recommended. If unset, leave 0 and let take_snapshot reject.
        int width = st->frame_width;
        int height = st->frame_height;

        std::lock_guard<std::mutex> lk(st->mtx);
        st->latest_frame.assign(payload, payload + payload_size);
        st->latest_width = width;
        st->latest_height = height;
        st->latest_stream_id = hdr->stream_id;
        st->latest_pts_usec = (int64_t)hdr->pts_usec;
        st->have_frame = true;
    }

    // Always forward downstream (pass-through).
    forward_downstream(st, buf, size);
}

// ---------------------------------------------------------------------------
// Lifecycle.
// ---------------------------------------------------------------------------
int handle_start(zm_plugin_t* plugin, zm_host_api_t* host, void* host_ctx,
                 const char* json_cfg) {
    zm_plugin_set_log_context(host, host_ctx);

    auto* st = new StoreSnapshotState();  // leaked on stop (see ctx)
    st->host = host;
    st->host_ctx = host_ctx;

    try {
        auto j = json::parse(json_cfg ? json_cfg : "{}");
        st->root = j.value("root", get_default_root());
        st->min_interval_ms = j.value("min_interval_ms", (int64_t)2000);
        st->jpeg_quality = j.value("jpeg_quality", 5);
        st->frame_width = j.value("frame_width", 0);
        st->frame_height = j.value("frame_height", 0);
        if (j.contains("trigger_types") && j["trigger_types"].is_array()) {
            for (const auto& t : j["trigger_types"])
                if (t.is_string())
                    st->trigger_types.push_back(t.get<std::string>());
        }
        if (j.contains("stream_filter") && j["stream_filter"].is_array()) {
            for (const auto& s : j["stream_filter"])
                if (s.is_number_unsigned() || s.is_number_integer())
                    st->stream_filter.push_back(s.get<uint32_t>());
        }
    } catch (...) {
        slog(st, ZM_LOG_ERROR, "store_snapshot: invalid config JSON");
        delete st;
        return -1;
    }
    if (st->jpeg_quality < 2) st->jpeg_quality = 2;
    if (st->jpeg_quality > 31) st->jpeg_quality = 31;
    if (st->min_interval_ms < 0) st->min_interval_ms = 0;

    st->running.store(true, std::memory_order_release);

    if (host && host->subscribe_evt)
        st->sub_handle = host->subscribe_evt(host_ctx, &event_cb, st);

    auto* ctx = new StoreSnapshotCtx{host, host_ctx, st};
    plugin->instance = ctx;

    slog(st, ZM_LOG_INFO,
         "store_snapshot: started root=%s min_interval_ms=%lld jpeg_quality=%d "
         "frame=%dx%d",
         st->root.c_str(), (long long)st->min_interval_ms, st->jpeg_quality,
         st->frame_width, st->frame_height);
    return 0;
}

void handle_stop(zm_plugin_t* plugin) {
    if (!plugin || !plugin->instance) return;
    auto* ctx = static_cast<StoreSnapshotCtx*>(plugin->instance);
    StoreSnapshotState* st = ctx->state;

    if (st) {
        // Stop callbacks first, then disarm any in-flight.
        if (ctx->host && ctx->host->unsubscribe_evt)
            ctx->host->unsubscribe_evt(ctx->host_ctx, st->sub_handle);
        st->running.store(false, std::memory_order_release);
    }

    // `st` is intentionally leaked so a late callback can't dangle.
    delete ctx;
    plugin->instance = nullptr;
}

}  // namespace

extern "C" __attribute__((visibility("default"))) void zm_plugin_init(
    zm_plugin_t* plugin) {
    if (!plugin) return;
    plugin->version = ZM_PLUGIN_ABI_VERSION;
    plugin->type = ZM_PLUGIN_PROCESS;
    plugin->instance = nullptr;
    plugin->start = handle_start;
    plugin->stop = handle_stop;
    plugin->on_frame = handle_frame;
}
