// store: unified ZoneMinder-Next recorder. One muxer + one zm-api event-id
// handshake, three recording policies selected by "mode":
//
//   "continuous" — record everything, rotating a segment every max_secs at a
//                  keyframe boundary. Each segment is a ZM event (cause
//                  "continuous"). (Replaces the old store_filesystem.)
//   "event"      — keep a rolling pre-roll buffer; on a trigger event write one
//                  clip with pre_roll_sec before and post_roll_sec after the last
//                  trigger. Each clip is a ZM event (cause = the trigger). The
//                  classic NVR event clip. (Replaces the old store_event.)
//   "both"       — continuous segments, but a trigger during a segment sets that
//                  segment's cause and its VLM descriptions are captured. No
//                  second file (ZM "Mocord").
//
// Every recorded clip/segment participates in the event-id handshake with zm-api
// over the worker socket: on open it emits recording_opening{clip_token,trigger};
// zm-api replies assign_recording{clip_token,event_id,dir,video_name}, which we
// apply by renaming the in-progress file (same filesystem; the open fd keeps
// writing the same inode). On close it emits EventClip{event_id,path,...}. If no
// reply arrives the clip keeps store's own naming (fallback). See the handshake
// docs in docs/.
//
// Data flow:
//   on_frame (capture thread): (a) maintain rolling buffer (event mode) / drive
//   segment open+rotate (continuous), (b) write to the open clip, (c) ALWAYS
//   forward downstream via host->on_frame.
//   event callback (publisher thread): StreamMetadata -> codec params; trigger
//   events -> start/extend a clip (event/both); assign_recording -> stash for the
//   capture thread; description -> sidecar.
//
// Lifetime: state is leaked on stop() so an in-flight host callback never dangles.

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
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

enum class Mode { Continuous, Event, Both };

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
struct StoreState {
    // Config.
    Mode mode = Mode::Continuous;
    std::string root;
    int monitor_id = 0;
    int max_secs = 300;                   // continuous: segment rotation length
    int pre_roll_sec = 5;                 // event: seconds before the trigger
    int post_roll_sec = 10;               // event: seconds after the last trigger
    int max_buffer_sec = 15;              // event: rolling-buffer cap
    std::vector<std::string> trigger_types;
    std::vector<uint32_t> stream_filter;  // empty == accept all

    // Host.
    zm_host_api_t* host = nullptr;
    void* host_ctx = nullptr;
    zm_plugin_t* plugin = nullptr;
    void* sub_handle = nullptr;

    // Lifetime gate for the host callback.
    std::atomic<bool> running{false};

    // Shared recording/buffer state — guarded by mtx (on_frame is on the capture
    // thread; the event callback is on the publisher thread).
    std::mutex mtx;

    VideoParams video;
    AudioParams audio;

    // Rolling pre-roll buffer (event mode only).
    std::deque<BufferedPacket> buffer;

    // Recording state.
    bool recording = false;
    int64_t last_trigger_usec = 0;
    bool warned_no_codec = false;

    // Open muxer.
    std::unique_ptr<AVFormatContext, decltype(&avformat_free_context)> fmt_ctx{
        nullptr, avformat_free_context};
    bool header_written = false;
    int64_t start_ts = 0;
    int64_t last_pts = 0;
    std::string cur_path;
    AVStream* video_stream = nullptr;
    AVStream* audio_stream = nullptr;

    // VLM "description" events captured during the current clip; written to a
    // sidecar JSON on close so recordings are searchable by description.
    std::vector<std::string> descriptions;

    // --- event-id assignment handshake with zm-api (via the worker socket) -----
    uint64_t clip_seq = 0;
    std::string current_clip_token;
    long current_event_id = 0;
    bool clip_assigned = false;
    int64_t frames_written = 0;
    std::string current_cause;            // "continuous" | trigger type

    // Pending assignment handed from the worker thread (event_cb) to the capture
    // thread (handle_frame). Guarded by its OWN leaf mutex so event_cb never takes
    // st->mtx while WorkerLink holds its lock (avoids a lock-order inversion).
    std::mutex assign_mtx;
    struct Assignment {
        bool valid = false;
        std::string clip_token;
        long event_id = 0;
        std::string dir;
        std::string video_name;
    };
    Assignment pending_assign;
};

struct StoreCtx {
    zm_host_api_t* host = nullptr;
    void* host_ctx = nullptr;
    StoreState* state = nullptr;
};

// ---------------------------------------------------------------------------
// Logging.
// ---------------------------------------------------------------------------
void slog(StoreState* st, zm_log_level_t level, const char* fmt, ...) {
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

Mode parse_mode(const std::string& s) {
    if (s == "event") return Mode::Event;
    if (s == "both") return Mode::Both;
    return Mode::Continuous;  // default
}

const char* mode_name(Mode m) {
    switch (m) {
        case Mode::Event: return "event";
        case Mode::Both: return "both";
        default: return "continuous";
    }
}

// "{root}/{YYYY-MM-DD}/event-Monitor-{stream}-{HH-MM-SS}.mkv" — store's own
// naming, used as the recording target until zm-api assigns the ZM-tree path.
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

bool stream_allowed(const StoreState* st, uint32_t sid) {
    if (st->stream_filter.empty()) return true;
    return std::find(st->stream_filter.begin(), st->stream_filter.end(), sid) !=
           st->stream_filter.end();
}

// ---------------------------------------------------------------------------
// Rolling buffer (event mode): append + keyframe-aligned trim. Caller holds mtx.
// ---------------------------------------------------------------------------
void buffer_append(StoreState* st, const zm_frame_hdr_t* hdr,
                   const uint8_t* payload) {
    BufferedPacket bp;
    bp.stream_id = hdr->stream_id;
    bp.hw_type = hdr->hw_type;
    bp.keyframe = (hdr->flags & 1u) != 0;
    bp.pts_usec = (int64_t)hdr->pts_usec;
    bp.data.assign(payload, payload + hdr->bytes);
    st->buffer.push_back(std::move(bp));

    int window_sec = st->pre_roll_sec + 2;
    if (window_sec > st->max_buffer_sec) window_sec = st->max_buffer_sec;
    if (window_sec < 1) window_sec = 1;
    const int64_t window_usec = (int64_t)window_sec * 1000000;

    const int64_t newest = st->buffer.back().pts_usec;
    const int64_t cutoff = newest - window_usec;

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

    const int64_t hard_cutoff = newest - (int64_t)st->max_buffer_sec * 1000000;
    while (st->buffer.size() > 1 && st->buffer.front().pts_usec < hard_cutoff) {
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
// Muxer open / write / close. Caller holds mtx.
// ---------------------------------------------------------------------------
bool open_clip(StoreState* st, uint32_t stream_id, std::time_t t) {
    if (!st->video.valid) {
        if (!st->warned_no_codec) {
            slog(st, ZM_LOG_WARN,
                 "store: recording requested before StreamMetadata known; "
                 "waiting for codec params");
            st->warned_no_codec = true;
        }
        return false;
    }

    st->cur_path = make_clip_path(st->root, st->monitor_id, stream_id, t);
    std::error_code ec;
    fs::create_directories(fs::path(st->cur_path).parent_path(), ec);
    // 1-second filename resolution: disambiguate same-second segments.
    if (fs::exists(st->cur_path)) {
        const std::string stem = st->cur_path.substr(0, st->cur_path.size() - 4);
        for (int n = 2;; ++n) {
            std::string cand = stem + "-" + std::to_string(n) + ".mkv";
            if (!fs::exists(cand)) { st->cur_path = cand; break; }
        }
    }

    AVFormatContext* ctx = nullptr;
    if (avformat_alloc_output_context2(&ctx, nullptr, "matroska",
                                       st->cur_path.c_str()) < 0 || !ctx) {
        slog(st, ZM_LOG_ERROR, "store: alloc output ctx failed for %s",
             st->cur_path.c_str());
        return false;
    }
    st->fmt_ctx.reset(ctx);

    if (avio_open(&ctx->pb, st->cur_path.c_str(), AVIO_FLAG_WRITE) < 0) {
        slog(st, ZM_LOG_ERROR, "store: avio_open failed for %s",
             st->cur_path.c_str());
        st->fmt_ctx.reset();
        return false;
    }

    AVStream* vst = avformat_new_stream(ctx, nullptr);
    if (!vst) {
        slog(st, ZM_LOG_ERROR, "store: could not create video stream");
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
        slog(st, ZM_LOG_ERROR, "store: write_header failed: %s", eb);
        avio_closep(&ctx->pb);
        st->fmt_ctx.reset();
        st->video_stream = nullptr;
        st->audio_stream = nullptr;
        return false;
    }

    st->header_written = true;
    st->start_ts = 0;
    st->last_pts = 0;
    st->frames_written = 0;
    slog(st, ZM_LOG_INFO, "store: opened clip %s", st->cur_path.c_str());
    return true;
}

void write_packet(StoreState* st, uint32_t hw_type, bool keyframe,
                  int64_t pts_usec, const uint8_t* data, size_t bytes) {
    if (!st->header_written || !st->fmt_ctx) return;

    AVStream* target = (hw_type == (uint32_t)ZM_FRAME_COMPRESSED_AUDIO)
                           ? st->audio_stream : st->video_stream;
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
        slog(st, ZM_LOG_ERROR, "store: write_frame failed: %s", eb);
    }
    if (hw_type != (uint32_t)ZM_FRAME_COMPRESSED_AUDIO) {
        st->last_pts = pts_usec;
        ++st->frames_written;
    }
    av_packet_free(&pkt);
}

void close_clip(StoreState* st) {
    if (!st->fmt_ctx) return;
    if (st->header_written) {
        av_write_trailer(st->fmt_ctx.get());
    }
    if (st->fmt_ctx->pb) avio_closep(&st->fmt_ctx->pb);

    int64_t duration = st->last_pts - st->start_ts;
    std::string path = st->cur_path;
    long ev_id = st->current_event_id;       // captured before the reset below
    int64_t frames = st->frames_written;
    std::string cause = st->current_cause;
    st->fmt_ctx.reset();
    st->header_written = false;
    st->video_stream = nullptr;
    st->audio_stream = nullptr;
    st->recording = false;
    st->start_ts = 0;
    st->last_pts = 0;
    st->frames_written = 0;
    st->current_clip_token.clear();
    st->current_event_id = 0;
    st->clip_assigned = false;
    st->current_cause.clear();

    // Sidecar JSON next to the clip — the VLM descriptions captured during the
    // event, so recordings are searchable/greppable by what was seen.
    {
        json side;
        side["clip"] = fs::path(path).filename().string();
        side["path"] = path;
        side["duration_usec"] = duration;
        side["cause"] = cause;
        side["descriptions"] = json::array();
        std::string joined;
        for (const auto& d : st->descriptions) {
            try {
                json dj = json::parse(d);
                side["descriptions"].push_back(dj);
                if (dj.contains("text") && dj["text"].is_string()) {
                    if (!joined.empty()) joined += " | ";
                    joined += dj["text"].get<std::string>();
                }
            } catch (const std::exception&) { /* skip malformed */ }
        }
        side["text"] = joined;
        const std::string side_path = path + ".json";
        std::ofstream f(side_path);
        if (f) { f << side.dump(2); }
        slog(st, ZM_LOG_INFO, "store: wrote sidecar %s (%zu descriptions)",
             side_path.c_str(), st->descriptions.size());
    }
    st->descriptions.clear();

    if (st->host && st->host->publish_evt) {
        // recording_saved: echo zm-api's assigned event_id (0 if unassigned), the
        // final path, cause, duration in seconds, and the video frame count.
        json ev = {{"event", "EventClip"},
                   {"event_id", ev_id},
                   {"path", path},
                   {"cause", cause},
                   {"duration", duration / 1e6},
                   {"frames", frames}};
        st->host->publish_evt(st->host_ctx, ev.dump().c_str());
    }
    slog(st, ZM_LOG_INFO,
         "store: closed clip %s (event_id=%ld, cause=%s, duration=%.2fs, frames=%lld)",
         path.c_str(), ev_id, cause.c_str(), duration / 1e6, (long long)frames);
}

void write_preroll(StoreState* st) {
    for (const auto& pk : st->buffer) {
        write_packet(st, pk.hw_type, pk.keyframe, pk.pts_usec, pk.data.data(),
                     pk.data.size());
    }
}

// ---------------------------------------------------------------------------
// Recording open + event-id handshake.
// ---------------------------------------------------------------------------
// Open a recording (segment or event clip) and start the zm-api event-id
// handshake. `cause` is the ZM event cause ("continuous" or the trigger type).
// Caller holds mtx.
bool open_recording(StoreState* st, uint32_t stream_id, const std::string& cause,
                    bool with_preroll) {
    std::time_t wall = std::time(nullptr);
    if (!open_clip(st, stream_id, wall)) return false;
    st->recording = true;
    st->descriptions.clear();
    st->current_cause = cause;
    st->current_clip_token = std::to_string(st->monitor_id) + "-" +
                             std::to_string((long long)wall) + "-" +
                             std::to_string(++st->clip_seq);
    st->current_event_id = 0;
    st->clip_assigned = false;

    if (with_preroll) write_preroll(st);

    // Ask zm-api to allocate the ZM event id + target path.
    if (st->host && st->host->publish_evt) {
        json req = {{"event", "RecordingOpening"},
                    {"clip_token", st->current_clip_token},
                    {"trigger", cause}};
        st->host->publish_evt(st->host_ctx, req.dump().c_str());
    }
    slog(st, ZM_LOG_INFO, "store: recording_opening token=%s cause=%s",
         st->current_clip_token.c_str(), cause.c_str());
    return true;
}

// Apply a pending event-id assignment from zm-api: rename the in-progress clip
// from store's own-naming path to zm-api's target. The open avio fd keeps writing
// the same inode after rename (POSIX, same filesystem); a cross-filesystem move
// fails and we keep the own-naming file as a fallback. Caller holds mtx.
void apply_pending_assignment(StoreState* st) {
    if (st->clip_assigned || !st->recording) return;
    StoreState::Assignment a;
    {
        std::lock_guard<std::mutex> lk(st->assign_mtx);
        if (!st->pending_assign.valid) return;
        if (st->pending_assign.clip_token != st->current_clip_token) return;
        a = st->pending_assign;
        st->pending_assign.valid = false;
    }
    st->current_event_id = a.event_id;
    st->clip_assigned = true;
    if (a.dir.empty() || a.video_name.empty()) return;  // id only, keep own naming

    std::error_code ec;
    fs::create_directories(a.dir, ec);
    const std::string target = a.dir + "/" + a.video_name;
    fs::rename(st->cur_path, target, ec);
    if (ec) {
        slog(st, ZM_LOG_WARN,
             "store: could not move clip to %s (%s); keeping %s",
             target.c_str(), ec.message().c_str(), st->cur_path.c_str());
    } else {
        slog(st, ZM_LOG_INFO, "store: assigned event_id=%ld, clip -> %s",
             a.event_id, target.c_str());
        st->cur_path = target;
    }
}

// StreamMetadata event -> configure codec params. Caller holds mtx.
void apply_stream_metadata(StoreState* st, const json& j) {
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
        slog(st, ZM_LOG_INFO, "store: audio metadata codec_id=%d rate=%d ch=%d",
             ap.codec_id, ap.sample_rate, ap.channels);
    } else {
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
        slog(st, ZM_LOG_INFO, "store: video metadata codec_id=%d %dx%d",
             vp.codec_id, vp.width, vp.height);
    }
}

void event_cb(void* user, const char* json_event) {
    auto* st = static_cast<StoreState*>(user);
    if (!st || !st->running.load(std::memory_order_acquire) || !json_event)
        return;

    json j;
    try {
        j = json::parse(json_event);
    } catch (...) {
        return;
    }

    // Inbound control command from zm-api, re-published on the bus by zm-core.
    // Stash under the leaf lock (NOT st->mtx) so it never blocks capture nor nests
    // st->mtx under WorkerLink's lock; handle_frame applies it on the capture thread.
    if (j.contains("cmd")) {
        if (j["cmd"] == "assign_recording") {
            std::lock_guard<std::mutex> lk(st->assign_mtx);
            st->pending_assign = StoreState::Assignment{
                /*valid=*/true,
                j.value("clip_token", std::string()),
                static_cast<long>(j.value("event_id", 0)),
                j.value("dir", std::string()),
                j.value("video_name", std::string())};
        }
        return;
    }

    if (j.value("event", std::string()) == "StreamMetadata") {
        std::lock_guard<std::mutex> lk(st->mtx);
        apply_stream_metadata(st, j);
        return;
    }

    std::string type;
    if (j.contains("type") && j["type"].is_string())
        type = j["type"].get<std::string>();

    // Capture VLM scene descriptions that land during an active clip.
    if (type == "description") {
        std::lock_guard<std::mutex> lk(st->mtx);
        if (st->recording && stream_allowed(st, j.value("stream_id", 0u)))
            st->descriptions.push_back(json_event);
        return;
    }

    // Continuous mode ignores triggers; segments are time-driven (handle_frame).
    if (st->mode == Mode::Continuous) return;
    if (!zm::storeevent::is_trigger(st->trigger_types, type)) return;

    uint32_t sid = j.value("stream_id", 0u);
    if (!stream_allowed(st, sid)) return;

    std::lock_guard<std::mutex> lk(st->mtx);
    int64_t now_usec = st->buffer.empty() ? st->last_pts : st->buffer.back().pts_usec;

    if (st->mode == Mode::Both) {
        // Continuous file is (or will be) recording; a trigger just sets the
        // segment's cause (and its descriptions are captured above).
        if (st->recording) st->current_cause = type;
        st->last_trigger_usec = now_usec;
        return;
    }

    // Event mode: a trigger starts a clip (writing pre-roll) or extends post-roll.
    if (!st->recording) {
        open_recording(st, sid, type, /*with_preroll=*/true);
    }
    st->last_trigger_usec = now_usec;
}

// ---------------------------------------------------------------------------
// Frame handling — capture thread.
// ---------------------------------------------------------------------------
void forward_downstream(StoreState* st, const void* buf, size_t size) {
    if (st->host && st->host->on_frame)
        st->host->on_frame(st->host_ctx, buf, size);
}

// Continuous/both: drive segment open + keyframe-aligned rotation. Caller holds
// mtx. Returns once the muxer is ready (or not yet) for this frame.
void drive_continuous(StoreState* st, const zm_frame_hdr_t* hdr) {
    const bool is_video = hdr->hw_type == (uint32_t)ZM_FRAME_COMPRESSED;
    const bool keyframe = (hdr->flags & 1u) != 0;
    if (!st->recording) {
        // Lazily open on the first video keyframe once codec params are known, so
        // the segment starts decodable and no orphan empty file is left.
        if (is_video && keyframe && st->video.valid)
            open_recording(st, hdr->stream_id, "continuous", /*with_preroll=*/false);
        return;
    }
    // Rotate at a keyframe boundary once the segment reaches max_secs.
    if (is_video && keyframe && st->start_ts != 0) {
        int64_t elapsed = ((int64_t)hdr->pts_usec - st->start_ts) / 1000000;
        if (elapsed >= st->max_secs) {
            close_clip(st);
            open_recording(st, hdr->stream_id, "continuous", /*with_preroll=*/false);
        }
    }
}

void handle_frame(zm_plugin_t* plugin, const void* buf, size_t size) {
    auto* ctx = static_cast<StoreCtx*>(plugin->instance);
    if (!ctx || !ctx->state || !buf) return;
    StoreState* st = ctx->state;

    if (size > 0 && static_cast<const char*>(buf)[0] == '{') {
        forward_downstream(st, buf, size);
        return;
    }
    if (size < sizeof(zm_frame_hdr_t)) {
        forward_downstream(st, buf, size);
        return;
    }

    const zm_frame_hdr_t* hdr = static_cast<const zm_frame_hdr_t*>(buf);
    bool is_video = hdr->hw_type == (uint32_t)ZM_FRAME_COMPRESSED;
    bool is_audio = hdr->hw_type == (uint32_t)ZM_FRAME_COMPRESSED_AUDIO;

    if ((is_video || is_audio) && hdr->bytes > 0 &&
        size >= sizeof(zm_frame_hdr_t) + hdr->bytes &&
        stream_allowed(st, hdr->stream_id)) {
        const uint8_t* payload =
            static_cast<const uint8_t*>(buf) + sizeof(zm_frame_hdr_t);

        std::lock_guard<std::mutex> lk(st->mtx);

        if (st->mode == Mode::Event) {
            // (a) keep the rolling pre-roll buffer.
            buffer_append(st, hdr, payload);
            // (b) if recording, apply any assignment, write, check post-roll.
            if (st->recording) {
                apply_pending_assignment(st);
                write_packet(st, hdr->hw_type, (hdr->flags & 1u) != 0,
                             (int64_t)hdr->pts_usec, payload, hdr->bytes);
                int64_t since = (int64_t)hdr->pts_usec - st->last_trigger_usec;
                if (since > (int64_t)st->post_roll_sec * 1000000)
                    close_clip(st);
            }
        } else {
            // continuous / both: open + rotate segments, write every frame.
            drive_continuous(st, hdr);
            if (st->recording) {
                apply_pending_assignment(st);
                write_packet(st, hdr->hw_type, (hdr->flags & 1u) != 0,
                             (int64_t)hdr->pts_usec, payload, hdr->bytes);
            }
        }
    }

    forward_downstream(st, buf, size);
}

// ---------------------------------------------------------------------------
// Lifecycle.
// ---------------------------------------------------------------------------
int handle_start(zm_plugin_t* plugin, zm_host_api_t* host, void* host_ctx,
                 const char* json_cfg) {
    zm_plugin_set_log_context(host, host_ctx);

    auto* st = new StoreState();  // leaked on stop (see StoreCtx)
    st->host = host;
    st->host_ctx = host_ctx;
    st->plugin = plugin;

    try {
        auto j = json::parse(json_cfg ? json_cfg : "{}");
        st->mode = parse_mode(j.value("mode", std::string("continuous")));
        st->root = j.value("root", get_default_root());
        st->monitor_id = j.value("monitor_id", 0);
        st->max_secs = j.value("max_secs", 300);
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
        slog(st, ZM_LOG_ERROR, "store: invalid config JSON");
        delete st;
        return -1;
    }
    if (st->max_buffer_sec < st->pre_roll_sec)
        st->max_buffer_sec = st->pre_roll_sec + 2;

    st->running.store(true, std::memory_order_release);

    if (host && host->subscribe_evt)
        st->sub_handle = host->subscribe_evt(host_ctx, &event_cb, st);

    auto* ctx = new StoreCtx{host, host_ctx, st};
    plugin->instance = ctx;

    slog(st, ZM_LOG_INFO,
         "store: started mode=%s root=%s max_secs=%d pre_roll=%d post_roll=%d",
         mode_name(st->mode), st->root.c_str(), st->max_secs,
         st->pre_roll_sec, st->post_roll_sec);
    return 0;
}

void handle_stop(zm_plugin_t* plugin) {
    if (!plugin || !plugin->instance) return;
    auto* ctx = static_cast<StoreCtx*>(plugin->instance);
    StoreState* st = ctx->state;

    if (st) {
        if (ctx->host && ctx->host->unsubscribe_evt)
            ctx->host->unsubscribe_evt(ctx->host_ctx, st->sub_handle);
        st->running.store(false, std::memory_order_release);

        std::lock_guard<std::mutex> lk(st->mtx);
        if (st->recording || st->fmt_ctx) close_clip(st);
    }

    delete ctx;  // `st` is intentionally leaked so a late callback can't dangle.
    plugin->instance = nullptr;
}

}  // namespace

extern "C" __attribute__((visibility("default"))) void zm_plugin_init(
    zm_plugin_t* plugin) {
    if (!plugin) return;
    plugin->version = ZM_PLUGIN_ABI_VERSION;
    plugin->type = ZM_PLUGIN_STORE;
    plugin->instance = nullptr;
    plugin->start = handle_start;
    plugin->stop = handle_stop;
    plugin->on_frame = handle_frame;
}
