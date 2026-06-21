// capture_file - ZM_PLUGIN_INPUT that replays a local media file's compressed
// video packets into the pipeline. A deterministic, camera-free sibling of
// capture_rtsp_multi: it demuxes (does NOT decode) and forwards compressed
// packets via host->on_frame, publishing a StreamMetadata handshake event so the
// downstream decoder can be configured.

#include "zm_plugin.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/base64.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/time.h>
}

namespace {

// Plugin context: holds FFmpeg state, config, and the replay thread.
struct CaptureFileContext {
    zm_host_api_t* host_api = nullptr;
    void* host_ctx = nullptr;

    // Configuration.
    std::string path;
    uint32_t stream_id = 0;
    bool loop = true;
    bool realtime = true;
    bool forward_audio = true;  // forward the file's audio stream too (if present)

    // FFmpeg state.
    AVFormatContext* fmt_ctx = nullptr;
    int video_stream_index = -1;
    int audio_stream_index = -1;

    // Replay thread control.
    std::thread worker;
    std::atomic<bool> running{false};

    uint64_t frames_emitted = 0;

    // Loop replay must present a MONOTONIC timeline: seeking back to the start
    // resets packet PTS to ~0, which would make downstream PTS go backwards (and
    // e.g. the muxer reject every frame with EINVAL). We add this accumulating
    // offset so each loop continues where the previous pass ended.
    int64_t pts_offset_usec = 0;
    int64_t last_emitted_pts_usec = 0;
    int64_t frame_dur_usec = 33333;  // refined from the stream's frame rate at open

    void log(zm_log_level_t level, const char* fmt, ...) {
        if (!host_api || !host_api->log) return;
        char buf[1024];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        host_api->log(host_ctx, level, buf);
    }
};

// Publish the StreamMetadata handshake event for a video or audio stream. The
// core turns it into the codec setup for downstream decode/store/Hello.
void publish_stream_metadata(CaptureFileContext* ctx, const AVCodecParameters* codecpar,
                             const char* media) {
    if (!ctx->host_api || !ctx->host_api->publish_evt || !codecpar) {
        return;
    }

    // Base64-encode extradata (SPS/PPS, AudioSpecificConfig, ...) if present.
    std::string extradata_b64;
    if (codecpar->extradata && codecpar->extradata_size > 0) {
        int b64len = 4 * ((codecpar->extradata_size + 2) / 3) + 1;
        std::vector<char> b64buf(b64len);
        av_base64_encode(b64buf.data(), b64len, codecpar->extradata, codecpar->extradata_size);
        extradata_b64 = std::string(b64buf.data());
    }

    // Build with nlohmann/json (NOT a fixed snprintf buffer): HEVC/AV1 extradata
    // base64 can exceed a kilobyte, and a truncated buffer yields invalid JSON that
    // every consumer's json::parse silently rejects (codec + extradata lost).
    nlohmann::json meta = {
        {"event", "StreamMetadata"},
        {"media", media},
        {"stream_id", ctx->stream_id},
        {"codec_id", static_cast<int>(codecpar->codec_id)},
        {"width", codecpar->width},
        {"height", codecpar->height},
        {"pix_fmt", codecpar->format},
        {"profile", codecpar->profile},
        {"level", codecpar->level},
        {"sample_rate", codecpar->sample_rate},
        {"channels", codecpar->ch_layout.nb_channels},
        {"extradata", extradata_b64},
    };
    ctx->host_api->publish_evt(ctx->host_ctx, meta.dump().c_str());

    ctx->log(ZM_LOG_INFO, "Published %s metadata for stream %u: codec=%s",
             media, ctx->stream_id,
             avcodec_get_name(static_cast<AVCodecID>(codecpar->codec_id)));
}

// Build [zm_frame_hdr_t][payload] and forward the packet downstream.
void emit_packet(CaptureFileContext* ctx, AVPacket* pkt, AVStream* stream, bool is_audio) {
    if (!ctx->host_api || !ctx->host_api->on_frame) return;
    if (!pkt->data || pkt->size <= 0) return;

    zm_frame_hdr_t hdr = {};
    hdr.stream_id = ctx->stream_id;
    hdr.hw_type = is_audio ? ZM_FRAME_COMPRESSED_AUDIO : ZM_FRAME_COMPRESSED;
    hdr.handle = reinterpret_cast<uint64_t>(pkt->data);
    hdr.bytes = static_cast<uint32_t>(pkt->size);
    hdr.flags = (pkt->flags & AV_PKT_FLAG_KEY) ? 1u : 0u;

    int64_t base_usec;
    if (pkt->pts != AV_NOPTS_VALUE && stream->time_base.den > 0) {
        base_usec = av_rescale_q(pkt->pts, stream->time_base, AVRational{1, 1000000});
    } else if (pkt->dts != AV_NOPTS_VALUE && stream->time_base.den > 0) {
        base_usec = av_rescale_q(pkt->dts, stream->time_base, AVRational{1, 1000000});
    } else {
        base_usec = av_gettime();
    }
    // Apply the loop offset so the timeline never runs backwards across replays.
    // The offset is driven by video (the primary stream); audio shares it to stay
    // aligned but does not redefine it.
    hdr.pts_usec = static_cast<uint64_t>(base_usec + ctx->pts_offset_usec);
    if (!is_audio) ctx->last_emitted_pts_usec = base_usec + ctx->pts_offset_usec;

    std::vector<uint8_t> frame_buf(sizeof(zm_frame_hdr_t) + pkt->size);
    std::memcpy(frame_buf.data(), &hdr, sizeof(zm_frame_hdr_t));
    std::memcpy(frame_buf.data() + sizeof(zm_frame_hdr_t), pkt->data, pkt->size);

    ctx->host_api->on_frame(ctx->host_ctx, frame_buf.data(), frame_buf.size());
    ctx->frames_emitted++;
}

// Replay loop: demux video packets and forward them, optionally pacing to PTS
// (realtime) and optionally looping back to the start on EOF.
void replay_loop(CaptureFileContext* ctx) {
    AVPacket* pkt = av_packet_alloc();
    if (!pkt) {
        ctx->log(ZM_LOG_ERROR, "Failed to allocate packet");
        return;
    }

    AVStream* stream = ctx->fmt_ctx->streams[ctx->video_stream_index];

    // Wall-clock anchor for realtime pacing. Reset each time we (re)start from
    // the beginning of the file so the first packet emits immediately.
    auto pace_origin = std::chrono::steady_clock::now();
    int64_t first_pts_usec = -1;

    ctx->log(ZM_LOG_INFO, "Starting replay loop for %s (realtime=%s, loop=%s)",
             ctx->path.c_str(), ctx->realtime ? "true" : "false",
             ctx->loop ? "true" : "false");

    while (ctx->running.load()) {
        int ret = av_read_frame(ctx->fmt_ctx, pkt);

        if (ret >= 0) {
            if (pkt->stream_index == ctx->video_stream_index) {
                // Pace to wall-clock if realtime replay is requested.
                if (ctx->realtime && pkt->pts != AV_NOPTS_VALUE &&
                    stream->time_base.den > 0) {
                    int64_t pts_usec =
                        av_rescale_q(pkt->pts, stream->time_base, AVRational{1, 1000000});
                    if (first_pts_usec < 0) {
                        first_pts_usec = pts_usec;
                    }
                    int64_t target_us = pts_usec - first_pts_usec;
                    auto now = std::chrono::steady_clock::now();
                    int64_t elapsed_us =
                        std::chrono::duration_cast<std::chrono::microseconds>(now - pace_origin)
                            .count();
                    int64_t sleep_us = target_us - elapsed_us;
                    if (sleep_us > 0) {
                        // Cap a single sleep so stop() stays responsive.
                        while (sleep_us > 0 && ctx->running.load()) {
                            int64_t chunk = sleep_us > 50000 ? 50000 : sleep_us;
                            std::this_thread::sleep_for(std::chrono::microseconds(chunk));
                            sleep_us -= chunk;
                        }
                    }
                }

                emit_packet(ctx, pkt, stream, /*is_audio=*/false);

                if (!ctx->realtime) {
                    // Small yield to avoid flooding downstream when running flat-out.
                    std::this_thread::sleep_for(std::chrono::microseconds(200));
                }

                if (ctx->frames_emitted % 300 == 0) {
                    ctx->log(ZM_LOG_DEBUG, "Stream %u: emitted %llu frames",
                             ctx->stream_id,
                             static_cast<unsigned long long>(ctx->frames_emitted));
                }
            } else if (ctx->forward_audio && pkt->stream_index == ctx->audio_stream_index) {
                // Audio rides the same pipeline (compressed), paced by the video
                // clock above. store/output consumers handle it via the audio path.
                emit_packet(ctx, pkt,
                            ctx->fmt_ctx->streams[ctx->audio_stream_index], /*is_audio=*/true);
            }
            av_packet_unref(pkt);
        } else if (ret == AVERROR_EOF) {
            if (ctx->loop) {
                ctx->log(ZM_LOG_INFO, "Stream %u: EOF, seeking to start (loop)",
                         ctx->stream_id);
                int seek_ret = av_seek_frame(ctx->fmt_ctx, ctx->video_stream_index, 0,
                                             AVSEEK_FLAG_BACKWARD);
                if (seek_ret < 0) {
                    char err_buf[AV_ERROR_MAX_STRING_SIZE];
                    av_strerror(seek_ret, err_buf, sizeof(err_buf));
                    ctx->log(ZM_LOG_ERROR, "Seek to start failed: %s, stopping", err_buf);
                    break;
                }
                // Continue the emitted timeline past the last frame of this pass so
                // downstream PTS stays monotonic across the loop boundary.
                ctx->pts_offset_usec = ctx->last_emitted_pts_usec + ctx->frame_dur_usec;
                // Re-anchor pacing for the new pass through the file.
                pace_origin = std::chrono::steady_clock::now();
                first_pts_usec = -1;
            } else {
                ctx->log(ZM_LOG_INFO, "Stream %u: EOF reached, stopping", ctx->stream_id);
                break;
            }
        } else if (ret == AVERROR(EAGAIN)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } else {
            char err_buf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, err_buf, sizeof(err_buf));
            ctx->log(ZM_LOG_WARN, "Stream %u: read error: %s, stopping",
                     ctx->stream_id, err_buf);
            break;
        }
    }

    av_packet_free(&pkt);
    ctx->log(ZM_LOG_INFO, "Replay loop ended for stream %u (%llu frames emitted)",
             ctx->stream_id, static_cast<unsigned long long>(ctx->frames_emitted));
}

// Open the file, find the first video stream, publish metadata.
bool open_input(CaptureFileContext* ctx) {
    int ret = avformat_open_input(&ctx->fmt_ctx, ctx->path.c_str(), nullptr, nullptr);
    if (ret < 0) {
        char err_buf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, err_buf, sizeof(err_buf));
        ctx->log(ZM_LOG_ERROR, "Failed to open file '%s': %s", ctx->path.c_str(), err_buf);
        return false;
    }

    ret = avformat_find_stream_info(ctx->fmt_ctx, nullptr);
    if (ret < 0) {
        char err_buf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, err_buf, sizeof(err_buf));
        ctx->log(ZM_LOG_ERROR, "Failed to find stream info: %s", err_buf);
        return false;
    }

    ctx->video_stream_index = -1;
    ctx->audio_stream_index = -1;
    for (unsigned int i = 0; i < ctx->fmt_ctx->nb_streams; i++) {
        const auto type = ctx->fmt_ctx->streams[i]->codecpar->codec_type;
        if (type == AVMEDIA_TYPE_VIDEO && ctx->video_stream_index < 0)
            ctx->video_stream_index = static_cast<int>(i);
        else if (type == AVMEDIA_TYPE_AUDIO && ctx->audio_stream_index < 0)
            ctx->audio_stream_index = static_cast<int>(i);
    }

    if (ctx->video_stream_index < 0) {
        ctx->log(ZM_LOG_ERROR, "No video stream found in '%s'", ctx->path.c_str());
        return false;
    }

    AVStream* video_stream = ctx->fmt_ctx->streams[ctx->video_stream_index];
    // Frame duration (for the loop-continuity offset) from the stream's frame rate.
    if (video_stream->avg_frame_rate.num > 0 && video_stream->avg_frame_rate.den > 0) {
        ctx->frame_dur_usec = av_rescale_q(1, av_inv_q(video_stream->avg_frame_rate),
                                           AVRational{1, 1000000});
    }
    publish_stream_metadata(ctx, video_stream->codecpar, "video");
    // Forward audio too (if present), so recordings/outputs can carry it.
    if (ctx->forward_audio && ctx->audio_stream_index >= 0) {
        publish_stream_metadata(
            ctx, ctx->fmt_ctx->streams[ctx->audio_stream_index]->codecpar, "audio");
    } else {
        ctx->audio_stream_index = -1;  // disabled
    }
    // Note: packets are forwarded in their native container form (AVCC for
    // MP4/MOV, Annex-B for MPEG-TS/RTSP). The StreamMetadata carries the codec
    // extradata so the decoder can parse AVCC and the store can mux it natively —
    // no in-pipeline bitstream conversion, so every consumer sees one consistent
    // stream.
    return true;
}

}  // namespace

// ----------------------------------------------------------------------------
// Plugin lifecycle
// ----------------------------------------------------------------------------

static int capture_file_start(zm_plugin_t* plugin, zm_host_api_t* host, void* host_ctx,
                              const char* json_cfg) {
    if (!plugin || !host || !json_cfg) {
        return -1;
    }

    zm_plugin_set_log_context(host, host_ctx);

    auto* ctx = new CaptureFileContext();
    ctx->host_api = host;
    ctx->host_ctx = host_ctx;
    plugin->instance = ctx;

    // Parse configuration with nlohmann/json.
    try {
        nlohmann::json cfg = nlohmann::json::parse(json_cfg);

        if (!cfg.contains("path") || !cfg["path"].is_string() ||
            cfg["path"].get<std::string>().empty()) {
            ctx->log(ZM_LOG_ERROR, "capture_file: required config key 'path' missing or empty");
            delete ctx;
            plugin->instance = nullptr;
            return -1;
        }
        ctx->path = cfg["path"].get<std::string>();

        if (cfg.contains("stream_id") && cfg["stream_id"].is_number_integer()) {
            ctx->stream_id = cfg["stream_id"].get<uint32_t>();
        }
        if (cfg.contains("loop") && cfg["loop"].is_boolean()) {
            ctx->loop = cfg["loop"].get<bool>();
        }
        if (cfg.contains("realtime") && cfg["realtime"].is_boolean()) {
            ctx->realtime = cfg["realtime"].get<bool>();
        }
        if (cfg.contains("forward_audio") && cfg["forward_audio"].is_boolean()) {
            ctx->forward_audio = cfg["forward_audio"].get<bool>();
        }
    } catch (const std::exception& e) {
        ctx->log(ZM_LOG_ERROR, "capture_file: failed to parse config: %s", e.what());
        delete ctx;
        plugin->instance = nullptr;
        return -1;
    }

    ctx->log(ZM_LOG_INFO,
             "Starting capture_file: path=%s stream_id=%u loop=%s realtime=%s",
             ctx->path.c_str(), ctx->stream_id, ctx->loop ? "true" : "false",
             ctx->realtime ? "true" : "false");

    if (!open_input(ctx)) {
        // Surface a health event so the orchestrator can mark the monitor
        // unhealthy instead of seeing a silent, frame-less worker (a dead input
        // would otherwise just stall with no signal).
        if (ctx->host_api && ctx->host_api->publish_evt) {
            nlohmann::json ev = {
                {"type", "connection_failed"},
                {"stream_id", ctx->stream_id},
                {"message", std::string("capture_file: failed to open ") + ctx->path},
            };
            ctx->host_api->publish_evt(ctx->host_ctx, ev.dump().c_str());
        }
        if (ctx->fmt_ctx) {
            avformat_close_input(&ctx->fmt_ctx);
        }
        delete ctx;
        plugin->instance = nullptr;
        return -1;
    }

    // Spawn the replay thread.
    ctx->running.store(true);
    ctx->worker = std::thread(replay_loop, ctx);

    ctx->log(ZM_LOG_INFO, "capture_file started successfully");
    return 0;
}

static void capture_file_stop(zm_plugin_t* plugin) {
    if (!plugin || !plugin->instance) {
        return;
    }

    auto* ctx = static_cast<CaptureFileContext*>(plugin->instance);
    ctx->log(ZM_LOG_INFO, "Stopping capture_file");

    ctx->running.store(false);
    if (ctx->worker.joinable()) {
        ctx->worker.join();
    }

    if (ctx->fmt_ctx) {
        avformat_close_input(&ctx->fmt_ctx);
        ctx->fmt_ctx = nullptr;
    }

    ctx->log(ZM_LOG_INFO, "capture_file stopped");

    delete ctx;
    plugin->instance = nullptr;
}

static void capture_file_on_frame(zm_plugin_t* plugin, const void* buf, size_t size) {
    // Input plugin: it produces frames, it does not receive them.
    (void)plugin;
    (void)buf;
    (void)size;
}

// ----------------------------------------------------------------------------
// Plugin entry point
// ----------------------------------------------------------------------------

extern "C" {

ZM_PLUGIN_EXPORT
void zm_plugin_init(zm_plugin_t* plugin) {
    if (!plugin) {
        return;
    }

    std::memset(plugin, 0, sizeof(zm_plugin_t));

    plugin->version = ZM_PLUGIN_ABI_VERSION;
    plugin->type = ZM_PLUGIN_INPUT;
    plugin->start = capture_file_start;
    plugin->stop = capture_file_stop;
    plugin->on_frame = capture_file_on_frame;
    plugin->instance = nullptr;
}

// Compatibility alias (matches capture_rtsp_multi).
ZM_PLUGIN_EXPORT
void init_plugin(zm_plugin_t* plugin) {
    zm_plugin_init(plugin);
}

}  // extern "C"
