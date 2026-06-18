// encode_ffmpeg.cpp - ZM_PLUGIN_PROCESS plugin for FFmpeg H.264 encoding.
// This is the inverse of decode_ffmpeg: it takes DECODED frames
// (RGB24 / GRAYSCALE / YUV420P) and H.264-encodes them into compressed
// packets, so processed frames (privacy-masked, overlaid) can be recorded
// or streamed. Frames that are already compressed, GPU surfaces, or audio
// are forwarded downstream unchanged (pass-through).
#include <zm_plugin.h>
#include <nlohmann/json.hpp>
#include <cstring>
#include <cstdio>
#include <vector>
#include <string>
#include <mutex>
#include <sstream>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
#include <libavutil/base64.h>
#include <libavutil/error.h>
}

static std::string get_av_error_string(int errnum) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(errnum, errbuf, AV_ERROR_MAX_STRING_SIZE);
    return std::string(errbuf);
}

static void log(zm_host_api_t* host, void* ctx, int lvl, const std::string& msg) {
    if (host && host->log) host->log(ctx, (zm_log_level_t)lvl, msg.c_str());
}

// Map a decoded-frame hw_type to its source AVPixelFormat for sws_scale input.
static AVPixelFormat hw_type_to_pix_fmt(uint32_t hw_type) {
    switch (hw_type) {
        case ZM_FRAME_RGB24:     return AV_PIX_FMT_RGB24;
        case ZM_FRAME_GRAYSCALE: return AV_PIX_FMT_GRAY8;
        case ZM_FRAME_YUV420P:   return AV_PIX_FMT_YUV420P;
        default:                 return AV_PIX_FMT_NONE;
    }
}

// Resolve an FFmpeg encoder name from a target codec + hwaccel. Lets users pick
// "codec":"h265","hwaccel":"nvenc" without knowing FFmpeg's encoder names. An
// explicit "encoder" config always overrides this.
static std::string resolve_encoder_name(const std::string& codec, const std::string& hw) {
    const bool h265 = (codec == "h265" || codec == "hevc");
    if (hw == "nvenc")        return h265 ? "hevc_nvenc" : "h264_nvenc";
    if (hw == "videotoolbox") return h265 ? "hevc_videotoolbox" : "h264_videotoolbox";
    if (hw == "vaapi")        return h265 ? "hevc_vaapi" : "h264_vaapi";
    if (hw == "qsv")          return h265 ? "hevc_qsv" : "h264_qsv";
    if (hw == "amf")          return h265 ? "hevc_amf" : "h264_amf";
    return h265 ? "libx265" : "libx264";   // software (none)
}

// Hardware encoders that accept a CPU NV12 input frame (FFmpeg uploads). vaapi/qsv
// generally need an explicit hw frames context (not yet wired) — documented.
static bool is_hw_encoder(const std::string& name) {
    return name.find("nvenc") != std::string::npos ||
           name.find("videotoolbox") != std::string::npos ||
           name.find("vaapi") != std::string::npos ||
           name.find("qsv") != std::string::npos ||
           name.find("amf") != std::string::npos;
}

struct EncoderCtx {
    // Config
    std::string encoder_name = "libx264";
    int64_t bitrate = 4000000;
    int gop = 50;
    int fps = 0;               // 0 => use microsecond time_base
    std::string preset = "veryfast";
    std::string tune = "zerolatency";
    int cfg_width = 0;
    int cfg_height = 0;
    int stream_filter = -1;    // -1 => all streams

    // FFmpeg state (lazily created on first frame)
    AVCodecContext* codec_ctx = nullptr;
    SwsContext* sws_ctx = nullptr;
    AVFrame* enc_frame = nullptr;       // reusable YUV420P input frame
    AVPacket* pkt = nullptr;
    int enc_width = 0;
    int enc_height = 0;
    AVPixelFormat src_pix_fmt = AV_PIX_FMT_NONE;
    AVRational time_base{1, 1000000};
    bool encoder_ready = false;
    bool metadata_published = false;

    zm_host_api_t* host = nullptr;
    void* host_ctx = nullptr;
    std::mutex mtx;
    uint64_t frames_encoded = 0;
    uint64_t packets_out = 0;
    int encode_errors = 0;

    ~EncoderCtx() {
        if (enc_frame) av_frame_free(&enc_frame);
        if (pkt) av_packet_free(&pkt);
        if (sws_ctx) sws_freeContext(sws_ctx);
        if (codec_ctx) avcodec_free_context(&codec_ctx);
    }
};

// Forward a single encoded AVPacket downstream as [zm_frame_hdr_t][bytes].
static void forward_packet(EncoderCtx* ctx, const AVPacket* pkt,
                           const zm_frame_hdr_t& in_hdr) {
    std::vector<uint8_t> out_buf(sizeof(zm_frame_hdr_t) + pkt->size);
    zm_frame_hdr_t out_hdr = in_hdr;
    out_hdr.hw_type = ZM_FRAME_COMPRESSED;
    out_hdr.handle = 0;
    out_hdr.bytes = (uint32_t)pkt->size;
    out_hdr.flags = (pkt->flags & AV_PKT_FLAG_KEY) ? 1u : 0u;
    // pkt->pts is in the encoder time_base; rescale back to microseconds.
    if (pkt->pts != AV_NOPTS_VALUE) {
        out_hdr.pts_usec = (uint64_t)av_rescale_q(
            pkt->pts, ctx->codec_ctx->time_base, AVRational{1, 1000000});
    }
    memcpy(out_buf.data(), &out_hdr, sizeof(zm_frame_hdr_t));
    memcpy(out_buf.data() + sizeof(zm_frame_hdr_t), pkt->data, pkt->size);
    if (ctx->host && ctx->host->on_frame)
        ctx->host->on_frame(ctx->host_ctx, out_buf.data(), out_buf.size());
    ctx->packets_out++;
}

// Drain all available packets from the encoder, forwarding each.
static void drain_packets(EncoderCtx* ctx, const zm_frame_hdr_t& in_hdr) {
    while (true) {
        int ret = avcodec_receive_packet(ctx->codec_ctx, ctx->pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) {
            ctx->encode_errors++;
            log(ctx->host, ctx->host_ctx, 3,
                "encode_ffmpeg: receive_packet failed: " + get_av_error_string(ret));
            break;
        }
        forward_packet(ctx, ctx->pkt, in_hdr);
        av_packet_unref(ctx->pkt);
    }
}

// Publish StreamMetadata so a downstream store learns the codec params.
// Shape matches capture_rtsp_multi exactly.
static void publish_stream_metadata(EncoderCtx* ctx, uint32_t stream_id) {
    if (!ctx->host || !ctx->host->publish_evt || !ctx->codec_ctx) return;

    AVCodecParameters* codecpar = avcodec_parameters_alloc();
    if (!codecpar) return;
    if (avcodec_parameters_from_context(codecpar, ctx->codec_ctx) < 0) {
        avcodec_parameters_free(&codecpar);
        return;
    }

    std::string extradata_b64;
    if (codecpar->extradata && codecpar->extradata_size > 0) {
        int b64len = 4 * ((codecpar->extradata_size + 2) / 3) + 1;
        std::vector<char> b64buf(b64len);
        av_base64_encode(b64buf.data(), b64len, codecpar->extradata,
                         codecpar->extradata_size);
        extradata_b64 = std::string(b64buf.data());
    }

    char metadata_json[2048];
    snprintf(metadata_json, sizeof(metadata_json),
             "{\"event\":\"StreamMetadata\",\"media\":\"video\",\"stream_id\":%u,"
             "\"codec_id\":%d,\"width\":%d,\"height\":%d,"
             "\"pix_fmt\":%d,\"profile\":%d,\"level\":%d,"
             "\"sample_rate\":%d,\"channels\":%d,"
             "\"extradata\":\"%s\"}",
             stream_id,
             (int)codecpar->codec_id,
             codecpar->width,
             codecpar->height,
             codecpar->format,
             codecpar->profile,
             codecpar->level,
             0,
             0,
             extradata_b64.c_str());

    ctx->host->publish_evt(ctx->host_ctx, metadata_json);
    avcodec_parameters_free(&codecpar);
}

// Lazily create and open the encoder using width/height/pixfmt derived from
// config + the first decoded frame. Returns 0 on success.
static int ensure_encoder(EncoderCtx* ctx, int width, int height,
                          AVPixelFormat src_pix_fmt) {
    if (ctx->encoder_ready) return 0;

    if (width <= 0 || height <= 0) {
        log(ctx->host, ctx->host_ctx, 3,
            "encode_ffmpeg: cannot determine encoder dimensions");
        return -1;
    }

    const AVCodec* codec = avcodec_find_encoder_by_name(ctx->encoder_name.c_str());
    if (!codec) {
        log(ctx->host, ctx->host_ctx, 2,
            "encode_ffmpeg: encoder '" + ctx->encoder_name +
            "' not found, falling back to default H264 encoder");
        codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    }
    if (!codec) {
        log(ctx->host, ctx->host_ctx, 3, "encode_ffmpeg: no H264 encoder available");
        return -1;
    }

    ctx->codec_ctx = avcodec_alloc_context3(codec);
    if (!ctx->codec_ctx) {
        log(ctx->host, ctx->host_ctx, 3, "encode_ffmpeg: failed to alloc codec ctx");
        return -1;
    }

    ctx->enc_width = width;
    ctx->enc_height = height;
    ctx->src_pix_fmt = src_pix_fmt;

    ctx->codec_ctx->width = width;
    ctx->codec_ctx->height = height;
    // HW encoders (nvenc/videotoolbox/...) prefer NV12 CPU input; software wants
    // YUV420P. The reusable input frame + swscale target this format.
    ctx->codec_ctx->pix_fmt = is_hw_encoder(ctx->encoder_name) ? AV_PIX_FMT_NV12
                                                               : AV_PIX_FMT_YUV420P;
    ctx->codec_ctx->bit_rate = ctx->bitrate;
    ctx->codec_ctx->gop_size = ctx->gop;
    if (ctx->fps > 0) {
        ctx->codec_ctx->time_base = AVRational{1, ctx->fps};
        ctx->codec_ctx->framerate = AVRational{ctx->fps, 1};
    } else {
        ctx->codec_ctx->time_base = AVRational{1, 1000000};
    }
    ctx->time_base = ctx->codec_ctx->time_base;

    // Emit global headers (extradata) so a downstream store/muxer can use them.
    ctx->codec_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    // x264-specific tuning.
    bool is_x264 = (ctx->encoder_name == "libx264") ||
                   (codec->name && std::string(codec->name) == "libx264");
    if (is_x264) {
        if (!ctx->preset.empty())
            av_opt_set(ctx->codec_ctx->priv_data, "preset", ctx->preset.c_str(), 0);
        if (!ctx->tune.empty())
            av_opt_set(ctx->codec_ctx->priv_data, "tune", ctx->tune.c_str(), 0);
    }

    int ret = avcodec_open2(ctx->codec_ctx, codec, nullptr);
    if (ret < 0) {
        log(ctx->host, ctx->host_ctx, 3,
            "encode_ffmpeg: failed to open encoder: " + get_av_error_string(ret));
        avcodec_free_context(&ctx->codec_ctx);
        return -1;
    }

    // Reusable input frame in the encoder's pixel format.
    ctx->enc_frame = av_frame_alloc();
    if (!ctx->enc_frame) {
        log(ctx->host, ctx->host_ctx, 3, "encode_ffmpeg: failed to alloc frame");
        avcodec_free_context(&ctx->codec_ctx);
        return -1;
    }
    ctx->enc_frame->format = ctx->codec_ctx->pix_fmt;
    ctx->enc_frame->width = width;
    ctx->enc_frame->height = height;
    if (av_frame_get_buffer(ctx->enc_frame, 0) < 0) {
        log(ctx->host, ctx->host_ctx, 3, "encode_ffmpeg: failed to alloc frame buffer");
        av_frame_free(&ctx->enc_frame);
        avcodec_free_context(&ctx->codec_ctx);
        return -1;
    }

    ctx->pkt = av_packet_alloc();
    if (!ctx->pkt) {
        log(ctx->host, ctx->host_ctx, 3, "encode_ffmpeg: failed to alloc packet");
        av_frame_free(&ctx->enc_frame);
        avcodec_free_context(&ctx->codec_ctx);
        return -1;
    }

    // swscale context: src (decoded pixfmt) -> encoder input pixfmt (YUV420P or NV12).
    ctx->sws_ctx = sws_getContext(width, height, src_pix_fmt,
                                  width, height, ctx->codec_ctx->pix_fmt,
                                  SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!ctx->sws_ctx) {
        log(ctx->host, ctx->host_ctx, 3, "encode_ffmpeg: failed to create swscale ctx");
        av_packet_free(&ctx->pkt);
        av_frame_free(&ctx->enc_frame);
        avcodec_free_context(&ctx->codec_ctx);
        return -1;
    }

    ctx->encoder_ready = true;
    std::ostringstream oss;
    oss << "encode_ffmpeg: opened encoder " << codec->name << " "
        << width << "x" << height << " bitrate=" << ctx->bitrate
        << " gop=" << ctx->gop;
    log(ctx->host, ctx->host_ctx, 4, oss.str());
    return 0;
}

static int process_start(zm_plugin_t* plugin, zm_host_api_t* host,
                         void* host_ctx, const char* json_cfg) {
    using json = nlohmann::json;
    zm_plugin_set_log_context(host, host_ctx);
    auto ctx = new EncoderCtx();
    ctx->host = host;
    ctx->host_ctx = host_ctx;

    try {
        auto cfg = json::parse(json_cfg ? json_cfg : "{}");
        // Output codec + hwaccel resolve to an encoder name, unless "encoder" is
        // given explicitly (which always wins).
        std::string codec = cfg.value("codec", std::string("h264"));
        std::string hwaccel = cfg.value("hwaccel", std::string("none"));
        std::string explicit_enc = cfg.value("encoder", std::string(""));
        ctx->encoder_name = explicit_enc.empty() ? resolve_encoder_name(codec, hwaccel)
                                                 : explicit_enc;
        ctx->bitrate = cfg.value("bitrate", (int64_t)4000000);
        ctx->gop = cfg.value("gop", 50);
        ctx->fps = cfg.value("fps", 0);
        ctx->preset = cfg.value("preset", std::string("veryfast"));
        ctx->tune = cfg.value("tune", std::string("zerolatency"));
        ctx->cfg_width = cfg.value("frame_width", 0);
        ctx->cfg_height = cfg.value("frame_height", 0);
        ctx->stream_filter = cfg.value("stream_filter", -1);
    } catch (...) {
        log(host, host_ctx, 3, "encode_ffmpeg: failed to parse config");
        delete ctx;
        return -1;
    }

    plugin->instance = ctx;
    log(host, host_ctx, 4, "encode_ffmpeg: started (encoder created lazily on first frame)");
    return 0;
}

static void process_on_frame(zm_plugin_t* plugin, const void* buf, size_t size) {
    if (!plugin || !plugin->instance || !buf || size < sizeof(zm_frame_hdr_t)) return;
    auto ctx = static_cast<EncoderCtx*>(plugin->instance);
    std::lock_guard<std::mutex> lock(ctx->mtx);
    const zm_frame_hdr_t* hdr = (const zm_frame_hdr_t*)buf;

    AVPixelFormat src_pix_fmt = hw_type_to_pix_fmt(hdr->hw_type);

    // Pass-through: already compressed, GPU surfaces, audio, or anything we
    // don't know how to encode. Forward unchanged so downstream consumers
    // still receive it.
    bool stream_match = (ctx->stream_filter < 0) ||
                        ((int)hdr->stream_id == ctx->stream_filter);
    if (src_pix_fmt == AV_PIX_FMT_NONE || !stream_match) {
        if (ctx->host && ctx->host->on_frame)
            ctx->host->on_frame(ctx->host_ctx, buf, size);
        return;
    }

    if (hdr->bytes == 0) return;
    if (size < sizeof(zm_frame_hdr_t) + hdr->bytes) return;

    // Determine dimensions: config overrides, else fall back to encoder dims.
    int width = ctx->cfg_width > 0 ? ctx->cfg_width : ctx->enc_width;
    int height = ctx->cfg_height > 0 ? ctx->cfg_height : ctx->enc_height;

    if (ensure_encoder(ctx, width, height, src_pix_fmt) != 0) {
        // Failed to create encoder; pass through so we don't drop the stream.
        if (ctx->host && ctx->host->on_frame)
            ctx->host->on_frame(ctx->host_ctx, buf, size);
        return;
    }
    width = ctx->enc_width;
    height = ctx->enc_height;

    if (!ctx->metadata_published) {
        publish_stream_metadata(ctx, hdr->stream_id);
        ctx->metadata_published = true;
    }

    const uint8_t* payload = (const uint8_t*)buf + sizeof(zm_frame_hdr_t);

    // Wrap the incoming payload as a source image for sws_scale.
    const uint8_t* src_data[4] = {nullptr, nullptr, nullptr, nullptr};
    int src_linesize[4] = {0, 0, 0, 0};
    int ret = av_image_fill_arrays(
        const_cast<uint8_t**>(src_data), src_linesize,
        payload, ctx->src_pix_fmt, width, height, 1);
    if (ret < 0) {
        log(ctx->host, ctx->host_ctx, 3, "encode_ffmpeg: failed to fill src arrays");
        return;
    }

    if (av_frame_make_writable(ctx->enc_frame) < 0) {
        log(ctx->host, ctx->host_ctx, 3, "encode_ffmpeg: frame not writable");
        return;
    }

    ret = sws_scale(ctx->sws_ctx, src_data, src_linesize, 0, height,
                    ctx->enc_frame->data, ctx->enc_frame->linesize);
    if (ret < 0) {
        log(ctx->host, ctx->host_ctx, 3, "encode_ffmpeg: sws_scale failed");
        return;
    }

    // pts: rescale incoming microseconds into the encoder time_base.
    ctx->enc_frame->pts = av_rescale_q(
        (int64_t)hdr->pts_usec, AVRational{1, 1000000}, ctx->codec_ctx->time_base);

    ret = avcodec_send_frame(ctx->codec_ctx, ctx->enc_frame);
    if (ret < 0) {
        ctx->encode_errors++;
        if (ctx->encode_errors % 10 == 1)
            log(ctx->host, ctx->host_ctx, 3,
                "encode_ffmpeg: send_frame failed: " + get_av_error_string(ret));
        return;
    }
    ctx->frames_encoded++;
    drain_packets(ctx, *hdr);

    if (ctx->frames_encoded % 100 == 0) {
        std::ostringstream oss;
        oss << "encode_ffmpeg: encoded " << ctx->frames_encoded << " frames, "
            << ctx->packets_out << " packets out, " << ctx->encode_errors << " errors";
        log(ctx->host, ctx->host_ctx, 4, oss.str());
    }
}

static void process_stop(zm_plugin_t* plugin) {
    if (!plugin || !plugin->instance) return;
    auto ctx = static_cast<EncoderCtx*>(plugin->instance);
    {
        std::lock_guard<std::mutex> lock(ctx->mtx);
        // Flush the encoder: send NULL and drain remaining packets.
        if (ctx->encoder_ready && ctx->codec_ctx) {
            if (avcodec_send_frame(ctx->codec_ctx, nullptr) >= 0) {
                zm_frame_hdr_t flush_hdr{};
                flush_hdr.hw_type = ZM_FRAME_COMPRESSED;
                drain_packets(ctx, flush_hdr);
            }
        }
    }
    delete ctx;
    plugin->instance = nullptr;
}

extern "C" __attribute__((visibility("default"))) void zm_plugin_init(zm_plugin_t* plugin) {
    if (!plugin) return;
    plugin->version = ZM_PLUGIN_ABI_VERSION;
    plugin->type = ZM_PLUGIN_PROCESS;
    plugin->instance = nullptr;
    plugin->start = process_start;
    plugin->stop = process_stop;
    plugin->on_frame = process_on_frame;
}
