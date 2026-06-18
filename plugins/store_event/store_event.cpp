// store_event: EVENT-BASED recorder with pre-roll and post-roll.
//
// Unlike store_filesystem (a continuous recorder), store_event is a
// ZM_PLUGIN_PROCESS pass-through that keeps a rolling in-memory buffer of recent
// compressed video (and audio) packets. When an alarm event fires (motion /
// detection / audio_event / tracked_detection — driven by the AI cascade rather
// than dumb motion), it writes a single clip file containing:
//
//   pre_roll_sec seconds BEFORE the trigger  ...through...  post_roll_sec
//   seconds AFTER the last trigger.
//
// This is the classic NVR "event clip".
//
// Data flow:
//   on_frame (capture thread):
//     (a) append the packet to the rolling buffer (keyframe-aligned trimming),
//     (b) if recording, write it to the open clip,
//     (c) ALWAYS forward downstream via host->on_frame (pass-through).
//   event callback (publisher thread, via host->subscribe_evt):
//     - StreamMetadata events configure the video/audio codec params.
//     - trigger events start a clip (writing pre-roll) and refresh last-trigger.
//   A trailing finalize happens lazily in on_frame once post_roll elapses.
//
// Lifetime: state is a raw, leaked struct handed to the host callback as `user`
// plus an atomic `running`; we unsubscribe in stop() and finalize any open clip.

#include "event_trigger.hpp"

#include <zm_plugin.h>
#include <nlohmann/json.hpp>

#ifdef __cplusplus
extern "C" {
#endif
#include <libavformat/avformat.h>
#include <libavutil/time.h>
#include <libavutil/avutil.h>
#include <libavcodec/avcodec.h>
#include <libavutil/base64.h>
#ifdef __cplusplus
}
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

// ---------------------------------------------------------------------------
// Codec parameters learned from StreamMetadata host events.
// ---------------------------------------------------------------------------
struct VideoParams {
    bool valid = false;
    int codec_id = 0;
    int width = 0;
    int height = 0;
    int format = 0;
    int profile = 0;
    int level = 0;
    std::vector<uint8_t> extradata;
};

struct AudioParams {
    bool valid = false;
    int codec_id = 0;
    int sample_rate = 0;
    int channels = 0;
    std::vector<uint8_t> extradata;
};

// One buffered compressed packet (copied off the host buffer).
struct BufferedPacket {
    uint32_t stream_id = 0;
    uint32_t hw_type = 0;     // ZM_FRAME_COMPRESSED or ZM_FRAME_COMPRESSED_AUDIO
    bool keyframe = false;
    int64_t pts_usec = 0;
    std::vector<uint8_t> data;
};

// ---------------------------------------------------------------------------
// Plugin state. Leaked on stop() so an in-flight host callback never dangles.
// ---------------------------------------------------------------------------
struct StoreEventState {
    // Config.
    std::string root;
    int monitor_id = 0;
    int pre_roll_sec = 5;
    int post_roll_sec = 10;
    int max_buffer_sec = 15;
    std::vector<std::string> trigger_types;
    std::vector<uint32_t> stream_filter;  // empty == accept all

    // Host.
    zm_host_api_t* host = nullptr;
    void* host_ctx = nullptr;
    zm_plugin_t* plugin = nullptr;        // for pass-through forwarding
    void* sub_handle = nullptr;

    // Lifetime gate for the host callback.
    std::atomic<bool> running{false};

    // Shared recording/buffer state — guarded by mtx (on_frame is on the capture
    // thread; the event callback is on the publisher thread).
    std::mutex mtx;

    // Codec params learned from StreamMetadata.
    VideoParams video;
    AudioParams audio;

    // Rolling pre-roll buffer (oldest at front). Keyframe-aligned trimming keeps
    // a clip decodable.
    std::deque<BufferedPacket> buffer;

    // Recording state.
    bool recording = false;
    int64_t last_trigger_usec = 0;        // wall-ish clock from frame pts
    bool warned_no_codec = false;         // log-once when a trigger arrives early

    // Open muxer.
    std::unique_ptr<AVFormatContext, decltype(&avformat_free_context)> fmt_ctx{
        nullptr, avformat_free_context};
    bool header_written = false;
    int64_t start_ts = 0;                 // pts_usec of first written packet
    int64_t last_pts = 0;
    std::string cur_path;
    AVStream* video_stream = nullptr;
    AVStream* audio_stream = nullptr;
};

// Couples plugin->instance to the (leaked) state.
struct StoreEventCtx {
    zm_host_api_t* host = nullptr;
    void* host_ctx = nullptr;
    StoreEventState* state = nullptr;
};

// ---------------------------------------------------------------------------
// Logging.
// ---------------------------------------------------------------------------
void slog(StoreEventState* st, zm_log_level_t level, const char* fmt, ...) {
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

// "{root}/{YYYY-MM-DD}/event-Monitor-{stream}-{HH-MM-SS}.mkv"
std::string make_clip_path(const std::string& root, int monitor_id,
                           uint32_t stream_id, std::time_t t) {
    std::tm tm = *std::localtime(&t);
    char dir[512];
    std::snprintf(dir, sizeof(dir), "%s/%04d-%02d-%02d",
                  root.c_str(), tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    char name[256];
    std::snprintf(name, sizeof(name), "event-Monitor-%d-%u-%02d-%02d-%02d.mkv",
                  monitor_id, stream_id, tm.tm_hour, tm.tm_min, tm.tm_sec);
    return std::string(dir) + "/" + std::string(name);
}

std::vector<uint8_t> b64_decode(const std::string& b64) {
    std::vector<uint8_t> out;
    if (b64.empty()) return out;
    size_t max_out = (b64.size() * 3) / 4 + 1;
    std::vector<uint8_t> tmp(max_out);
    int n = av_base64_decode(tmp.data(), b64.c_str(), (int)max_out);
    if (n > 0) {
        tmp.resize(n);
        out = std::move(tmp);
    }
    return out;
}

bool stream_allowed(const StoreEventState* st, uint32_t sid) {
    if (st->stream_filter.empty()) return true;
    return std::find(st->stream_filter.begin(), st->stream_filter.end(), sid) !=
           st->stream_filter.end();
}

// ---------------------------------------------------------------------------
// Rolling buffer: append + keyframe-aligned trim. Caller holds st->mtx.
// ---------------------------------------------------------------------------
void buffer_append(StoreEventState* st, const zm_frame_hdr_t* hdr,
                   const uint8_t* payload) {
    BufferedPacket bp;
    bp.stream_id = hdr->stream_id;
    bp.hw_type = hdr->hw_type;
    bp.keyframe = (hdr->flags & 1u) != 0;
    bp.pts_usec = (int64_t)hdr->pts_usec;
    bp.data.assign(payload, payload + hdr->bytes);
    st->buffer.push_back(std::move(bp));

    // Trim by duration. Window = pre_roll + a margin; never trim beyond
    // max_buffer_sec. We must not drop past the last keyframe that precedes the
    // retained window, so a future clip starts decodable.
    int window_sec = st->pre_roll_sec + 2;
    if (window_sec > st->max_buffer_sec) window_sec = st->max_buffer_sec;
    if (window_sec < 1) window_sec = 1;
    const int64_t window_usec = (int64_t)window_sec * 1000000;

    const int64_t newest = st->buffer.back().pts_usec;
    const int64_t cutoff = newest - window_usec;

    // Find the index of the last keyframe at or before `cutoff`; everything
    // strictly before it can be dropped. If no such keyframe exists, keep all.
    size_t drop_before = 0;
    bool found = false;
    for (size_t i = 0; i < st->buffer.size(); ++i) {
        const auto& pk = st->buffer[i];
        if (pk.pts_usec > cutoff) break;
        if (pk.hw_type == (uint32_t)ZM_FRAME_COMPRESSED && pk.keyframe) {
            drop_before = i;
            found = true;
        }
    }
    if (found) {
        for (size_t i = 0; i < drop_before; ++i)
            st->buffer.pop_front();
    }

    // Hard cap against runaway memory: if still older than max_buffer_sec, drop
    // from the front to the next video keyframe.
    const int64_t hard_cutoff = newest - (int64_t)st->max_buffer_sec * 1000000;
    while (st->buffer.size() > 1 && st->buffer.front().pts_usec < hard_cutoff) {
        // Only drop if doing so still leaves a keyframe to start from.
        bool keyframe_ahead = false;
        for (size_t i = 1; i < st->buffer.size(); ++i) {
            if (st->buffer[i].hw_type == (uint32_t)ZM_FRAME_COMPRESSED &&
                st->buffer[i].keyframe) {
                keyframe_ahead = true;
                break;
            }
        }
        if (!keyframe_ahead) break;
        st->buffer.pop_front();
    }
}

// ---------------------------------------------------------------------------
// Muxer open / write / close. Caller holds st->mtx.
// ---------------------------------------------------------------------------
bool open_clip(StoreEventState* st, uint32_t stream_id, std::time_t t) {
    if (!st->video.valid) {
        if (!st->warned_no_codec) {
            slog(st, ZM_LOG_WARN,
                 "store_event: trigger before StreamMetadata known; cannot open "
                 "clip yet (waiting for codec params)");
            st->warned_no_codec = true;
        }
        return false;
    }

    st->cur_path = make_clip_path(st->root, st->monitor_id, stream_id, t);
    std::error_code ec;
    fs::create_directories(fs::path(st->cur_path).parent_path(), ec);

    AVFormatContext* ctx = nullptr;
    if (avformat_alloc_output_context2(&ctx, nullptr, "matroska",
                                       st->cur_path.c_str()) < 0 || !ctx) {
        slog(st, ZM_LOG_ERROR, "store_event: alloc output ctx failed for %s",
             st->cur_path.c_str());
        return false;
    }
    st->fmt_ctx.reset(ctx);

    if (avio_open(&ctx->pb, st->cur_path.c_str(), AVIO_FLAG_WRITE) < 0) {
        slog(st, ZM_LOG_ERROR, "store_event: avio_open failed for %s",
             st->cur_path.c_str());
        st->fmt_ctx.reset();
        return false;
    }

    // Video stream.
    AVStream* vst = avformat_new_stream(ctx, nullptr);
    if (!vst) {
        slog(st, ZM_LOG_ERROR, "store_event: could not create video stream");
        avio_closep(&ctx->pb);
        st->fmt_ctx.reset();
        return false;
    }
    vst->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    vst->codecpar->codec_id = (enum AVCodecID)st->video.codec_id;
    vst->codecpar->width = st->video.width;
    vst->codecpar->height = st->video.height;
    vst->codecpar->format = st->video.format;
    vst->codecpar->profile = st->video.profile;
    vst->codecpar->level = st->video.level;
    if (!st->video.extradata.empty()) {
        uint8_t* ed = (uint8_t*)av_mallocz(st->video.extradata.size() +
                                           AV_INPUT_BUFFER_PADDING_SIZE);
        if (ed) {
            memcpy(ed, st->video.extradata.data(), st->video.extradata.size());
            vst->codecpar->extradata = ed;
            vst->codecpar->extradata_size = (int)st->video.extradata.size();
        }
    }
    vst->time_base = AVRational{1, 1000000};
    vst->avg_frame_rate = AVRational{25, 1};
    vst->r_frame_rate = vst->avg_frame_rate;
    st->video_stream = vst;

    // Audio stream (best-effort, must be added before write_header).
    st->audio_stream = nullptr;
    if (st->audio.valid) {
        AVStream* ast = avformat_new_stream(ctx, nullptr);
        if (ast) {
            ast->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
            ast->codecpar->codec_id = (enum AVCodecID)st->audio.codec_id;
            ast->codecpar->sample_rate = st->audio.sample_rate;
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 100)
            av_channel_layout_default(
                &ast->codecpar->ch_layout,
                st->audio.channels > 0 ? st->audio.channels : 1);
#else
            ast->codecpar->channels = st->audio.channels;
            ast->codecpar->channel_layout = av_get_default_channel_layout(
                st->audio.channels > 0 ? st->audio.channels : 1);
#endif
            if (!st->audio.extradata.empty()) {
                uint8_t* ed = (uint8_t*)av_mallocz(st->audio.extradata.size() +
                                                   AV_INPUT_BUFFER_PADDING_SIZE);
                if (ed) {
                    memcpy(ed, st->audio.extradata.data(),
                           st->audio.extradata.size());
                    ast->codecpar->extradata = ed;
                    ast->codecpar->extradata_size =
                        (int)st->audio.extradata.size();
                }
            }
            ast->time_base = AVRational{1, 1000000};
            st->audio_stream = ast;
        }
    }

    int ret = avformat_write_header(ctx, nullptr);
    if (ret < 0) {
        char eb[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, eb, sizeof(eb));
        slog(st, ZM_LOG_ERROR, "store_event: write_header failed: %s", eb);
        avio_closep(&ctx->pb);
        st->fmt_ctx.reset();
        st->video_stream = nullptr;
        st->audio_stream = nullptr;
        return false;
    }

    st->header_written = true;
    st->start_ts = 0;
    st->last_pts = 0;
    slog(st, ZM_LOG_INFO, "store_event: opened clip %s", st->cur_path.c_str());
    return true;
}

void write_packet(StoreEventState* st, uint32_t hw_type, bool keyframe,
                  int64_t pts_usec, const uint8_t* data, size_t bytes) {
    if (!st->header_written || !st->fmt_ctx) return;

    AVStream* target = nullptr;
    if (hw_type == (uint32_t)ZM_FRAME_COMPRESSED_AUDIO) {
        target = st->audio_stream;  // best-effort; null => drop audio
    } else {
        target = st->video_stream;
    }
    if (!target) return;

    if (st->start_ts == 0) st->start_ts = pts_usec;
    int64_t rel = pts_usec - st->start_ts;
    if (rel < 0) rel = 0;

    AVPacket* pkt = av_packet_alloc();
    if (!pkt) return;
    if (av_new_packet(pkt, (int)bytes) < 0) {
        av_packet_free(&pkt);
        return;
    }
    memcpy(pkt->data, data, bytes);
    pkt->pts = pkt->dts =
        av_rescale_q(rel, AVRational{1, 1000000}, target->time_base);
    pkt->stream_index = target->index;
    if (keyframe || hw_type == (uint32_t)ZM_FRAME_COMPRESSED_AUDIO)
        pkt->flags |= AV_PKT_FLAG_KEY;

    int ret = av_interleaved_write_frame(st->fmt_ctx.get(), pkt);
    if (ret < 0) {
        char eb[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, eb, sizeof(eb));
        slog(st, ZM_LOG_ERROR, "store_event: write_frame failed: %s", eb);
    }
    if (hw_type != (uint32_t)ZM_FRAME_COMPRESSED_AUDIO)
        st->last_pts = pts_usec;
    av_packet_free(&pkt);
}

void close_clip(StoreEventState* st) {
    if (!st->fmt_ctx) return;
    if (st->header_written) {
        av_write_trailer(st->fmt_ctx.get());
    }
    if (st->fmt_ctx->pb) avio_closep(&st->fmt_ctx->pb);

    int64_t duration = st->last_pts - st->start_ts;
    std::string path = st->cur_path;
    st->fmt_ctx.reset();
    st->header_written = false;
    st->video_stream = nullptr;
    st->audio_stream = nullptr;
    st->recording = false;
    st->start_ts = 0;
    st->last_pts = 0;

    if (st->host && st->host->publish_evt) {
        json ev = {{"event", "EventClip"}, {"path", path}, {"duration", duration}};
        st->host->publish_evt(st->host_ctx, ev.dump().c_str());
    }
    slog(st, ZM_LOG_INFO, "store_event: closed clip %s (duration=%lld)",
         path.c_str(), (long long)duration);
}

// Flush the entire rolling buffer (pre-roll) into the freshly opened clip. The
// buffer already starts at/before a video keyframe. Caller holds st->mtx.
void write_preroll(StoreEventState* st) {
    for (const auto& pk : st->buffer) {
        write_packet(st, pk.hw_type, pk.keyframe, pk.pts_usec, pk.data.data(),
                     pk.data.size());
    }
}

// ---------------------------------------------------------------------------
// Trigger handling — invoked from the host event callback.
// ---------------------------------------------------------------------------
void start_recording(StoreEventState* st, uint32_t stream_id, int64_t now_usec) {
    std::time_t wall = std::time(nullptr);
    if (!open_clip(st, stream_id, wall)) {
        return;  // codec params not ready, etc. (already logged)
    }
    st->recording = true;
    write_preroll(st);
    st->last_trigger_usec = now_usec;
}

// StreamMetadata event -> configure codec params. Caller holds st->mtx.
void apply_stream_metadata(StoreEventState* st, const json& j) {
    std::string media = j.value("media", std::string());
    uint32_t sid = j.value("stream_id", 0u);
    if (!stream_allowed(st, sid)) return;

    if (media == "audio") {
        AudioParams ap;
        ap.codec_id = j.value("codec_id", 0);
        ap.sample_rate = j.value("sample_rate", 0);
        ap.channels = j.value("channels", 0);
        ap.extradata = b64_decode(j.value("extradata", std::string()));
        ap.valid = true;
        st->audio = std::move(ap);
        slog(st, ZM_LOG_INFO,
             "store_event: audio metadata codec_id=%d rate=%d ch=%d",
             ap.codec_id, ap.sample_rate, ap.channels);
    } else {
        // Treat anything non-audio (including unset media) as video.
        VideoParams vp;
        vp.codec_id = j.value("codec_id", 0);
        vp.width = j.value("width", 0);
        vp.height = j.value("height", 0);
        vp.format = j.value("pix_fmt", 0);
        vp.profile = j.value("profile", 0);
        vp.level = j.value("level", 0);
        vp.extradata = b64_decode(j.value("extradata", std::string()));
        vp.valid = true;
        st->video = std::move(vp);
        slog(st, ZM_LOG_INFO,
             "store_event: video metadata codec_id=%d %dx%d",
             vp.codec_id, vp.width, vp.height);
    }
}

void event_cb(void* user, const char* json_event) {
    auto* st = static_cast<StoreEventState*>(user);
    if (!st || !st->running.load(std::memory_order_acquire) || !json_event)
        return;

    json j;
    try {
        j = json::parse(json_event);
    } catch (...) {
        return;
    }

    // StreamMetadata configures the encoder/stream.
    if (j.value("event", std::string()) == "StreamMetadata") {
        std::lock_guard<std::mutex> lk(st->mtx);
        apply_stream_metadata(st, j);
        return;
    }

    // Trigger detection: an event "type" in the configured trigger set.
    std::string type;
    if (j.contains("type") && j["type"].is_string())
        type = j["type"].get<std::string>();
    if (!zm::storeevent::is_trigger(st->trigger_types, type))
        return;

    uint32_t sid = j.value("stream_id", 0u);
    if (!stream_allowed(st, sid)) return;

    std::lock_guard<std::mutex> lk(st->mtx);
    // Use the newest buffered frame's clock as "now" (frames carry the canonical
    // microsecond clock); fall back to 0 if the buffer is empty.
    int64_t now_usec = st->buffer.empty() ? 0 : st->buffer.back().pts_usec;

    if (!st->recording) {
        start_recording(st, sid, now_usec);
    } else {
        st->last_trigger_usec = now_usec;  // extend the post-roll window
    }
}

// ---------------------------------------------------------------------------
// Frame handling — capture thread.
// ---------------------------------------------------------------------------
void forward_downstream(StoreEventState* st, const void* buf, size_t size) {
    if (st->host && st->host->on_frame)
        st->host->on_frame(st->host_ctx, buf, size);
}

void handle_frame(zm_plugin_t* plugin, const void* buf, size_t size) {
    auto* ctx = static_cast<StoreEventCtx*>(plugin->instance);
    if (!ctx || !ctx->state || !buf) return;
    StoreEventState* st = ctx->state;

    // JSON metadata events may also arrive inline as frames (buffer starts '{').
    // We rely on the host event subscription for codec params, so just forward.
    if (size > 0 && static_cast<const char*>(buf)[0] == '{') {
        forward_downstream(st, buf, size);
        return;
    }

    if (size < sizeof(zm_frame_hdr_t)) {
        forward_downstream(st, buf, size);
        return;
    }

    const zm_frame_hdr_t* hdr = static_cast<const zm_frame_hdr_t*>(buf);

    // Only buffer/record compressed CPU media; everything still forwards.
    bool is_video = hdr->hw_type == (uint32_t)ZM_FRAME_COMPRESSED;
    bool is_audio = hdr->hw_type == (uint32_t)ZM_FRAME_COMPRESSED_AUDIO;

    if ((is_video || is_audio) && hdr->bytes > 0 &&
        size >= sizeof(zm_frame_hdr_t) + hdr->bytes &&
        stream_allowed(st, hdr->stream_id)) {
        const uint8_t* payload =
            static_cast<const uint8_t*>(buf) + sizeof(zm_frame_hdr_t);

        std::lock_guard<std::mutex> lk(st->mtx);

        // (a) append to rolling buffer.
        buffer_append(st, hdr, payload);

        // (b) if recording, write this packet, and check post-roll expiry.
        if (st->recording) {
            write_packet(st, hdr->hw_type, (hdr->flags & 1u) != 0,
                         (int64_t)hdr->pts_usec, payload, hdr->bytes);

            int64_t now_usec = (int64_t)hdr->pts_usec;
            int64_t since = now_usec - st->last_trigger_usec;
            if (since > (int64_t)st->post_roll_sec * 1000000) {
                close_clip(st);
            }
        }
    }

    // (c) always forward downstream (pass-through).
    forward_downstream(st, buf, size);
}

// ---------------------------------------------------------------------------
// Lifecycle.
// ---------------------------------------------------------------------------
int handle_start(zm_plugin_t* plugin, zm_host_api_t* host, void* host_ctx,
                 const char* json_cfg) {
    zm_plugin_set_log_context(host, host_ctx);

    auto* st = new StoreEventState();  // leaked on stop (see StoreEventCtx)
    st->host = host;
    st->host_ctx = host_ctx;
    st->plugin = plugin;

    try {
        auto j = json::parse(json_cfg ? json_cfg : "{}");
        st->root = j.value("root", get_default_root());
        st->monitor_id = j.value("monitor_id", 0);
        st->pre_roll_sec = j.value("pre_roll_sec", 5);
        st->post_roll_sec = j.value("post_roll_sec", 10);
        st->max_buffer_sec = j.value("max_buffer_sec", 15);
        if (j.contains("trigger_types") && j["trigger_types"].is_array()) {
            for (const auto& t : j["trigger_types"])
                if (t.is_string()) st->trigger_types.push_back(t.get<std::string>());
        }
        if (j.contains("stream_filter") && j["stream_filter"].is_array()) {
            for (const auto& s : j["stream_filter"])
                if (s.is_number_unsigned())
                    st->stream_filter.push_back(s.get<uint32_t>());
        }
    } catch (...) {
        slog(st, ZM_LOG_ERROR, "store_event: invalid config JSON");
        delete st;
        return -1;
    }
    if (st->max_buffer_sec < st->pre_roll_sec)
        st->max_buffer_sec = st->pre_roll_sec + 2;

    st->running.store(true, std::memory_order_release);

    if (host && host->subscribe_evt)
        st->sub_handle = host->subscribe_evt(host_ctx, &event_cb, st);

    auto* ctx = new StoreEventCtx{host, host_ctx, st};
    plugin->instance = ctx;

    slog(st, ZM_LOG_INFO,
         "store_event: started root=%s pre_roll=%d post_roll=%d max_buffer=%d",
         st->root.c_str(), st->pre_roll_sec, st->post_roll_sec,
         st->max_buffer_sec);
    return 0;
}

void handle_stop(zm_plugin_t* plugin) {
    if (!plugin || !plugin->instance) return;
    auto* ctx = static_cast<StoreEventCtx*>(plugin->instance);
    StoreEventState* st = ctx->state;

    if (st) {
        // Stop callbacks first, then disarm any in-flight.
        if (ctx->host && ctx->host->unsubscribe_evt)
            ctx->host->unsubscribe_evt(ctx->host_ctx, st->sub_handle);
        st->running.store(false, std::memory_order_release);

        std::lock_guard<std::mutex> lk(st->mtx);
        if (st->recording || st->fmt_ctx) close_clip(st);
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
