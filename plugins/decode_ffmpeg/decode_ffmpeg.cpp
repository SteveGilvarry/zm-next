// decode_ffmpeg.cpp - ZM_PLUG_PROCESS plugin for FFmpeg decoding
#include <zm_plugin.h>
#include <nlohmann/json.hpp>
#include <cstring>
#include <vector>
#include <string>
#include <memory>
#include <mutex>
#include <thread>
#include <atomic>
#include <sstream>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

struct DecoderCtx {
    int threads = 0;
    std::string scale = "orig";
    bool hw_decode = false;
    AVCodecContext* codec_ctx = nullptr;
    SwsContext* sws_ctx = nullptr;
    int out_width = 0, out_height = 0;
    AVPixelFormat out_pix_fmt = AV_PIX_FMT_YUV420P;
    std::vector<uint8_t> yuv_buf;
    zm_host_api_t* host = nullptr;
    void* host_ctx = nullptr;
    std::mutex mtx;
    std::atomic<bool> running{true};
    ~DecoderCtx() {
        if (codec_ctx) avcodec_free_context(&codec_ctx);
        if (sws_ctx) sws_freeContext(sws_ctx);
    }
};

static void log(zm_host_api_t* host, void* ctx, int lvl, const std::string& msg) {
    if (host && host->log) host->log(ctx, (zm_log_level_t)lvl, msg.c_str());
}

static int process_start(zm_plugin_t* plugin, zm_host_api_t* host, void* host_ctx, const char* json_cfg) {
    using json = nlohmann::json;
    auto ctx = new DecoderCtx();
    ctx->host = host;
    ctx->host_ctx = host_ctx;
    // Parse config
    try {
        auto cfg = json::parse(json_cfg ? json_cfg : "{}");
        ctx->threads = cfg.value("threads", 0);
        ctx->scale = cfg.value("scale", "orig");
        ctx->hw_decode = cfg.value("hw_decode", false);
    } catch (...) {
        log(host, host_ctx, 3, "decode_ffmpeg: failed to parse config");
        delete ctx;
        return -1;
    }
    // Only H264 for now
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
        log(host, host_ctx, 3, "decode_ffmpeg: H264 decoder not found");
        delete ctx;
        return -1;
    }
    ctx->codec_ctx = avcodec_alloc_context3(codec);
    if (!ctx->codec_ctx) {
        log(host, host_ctx, 3, "decode_ffmpeg: failed to alloc codec ctx");
        delete ctx;
        return -1;
    }
    ctx->codec_ctx->thread_count = ctx->threads;
    ctx->codec_ctx->thread_type = ctx->threads > 0 ? FF_THREAD_FRAME : 0;
    if (avcodec_open2(ctx->codec_ctx, codec, nullptr) < 0) {
        log(host, host_ctx, 3, "decode_ffmpeg: failed to open codec");
        delete ctx;
        return -1;
    }
    std::ostringstream oss;
    oss << "decode_ffmpeg: created decoder H264 size "
        << ctx->codec_ctx->width << "x" << ctx->codec_ctx->height;
    log(host, host_ctx, 4, oss.str());
    plugin->instance = ctx;
    return 0;
}

static void process_stop(zm_plugin_t* plugin) {
    if (!plugin || !plugin->instance) return;
    auto ctx = static_cast<DecoderCtx*>(plugin->instance);
    ctx->running = false;
    delete ctx;
    plugin->instance = nullptr;
}

static void process_on_frame(zm_plugin_t* plugin, const zm_frame_hdr_t* hdr, const void* frame, size_t size) {
    if (!plugin || !plugin->instance || !hdr) return;
    auto ctx = static_cast<DecoderCtx*>(plugin->instance);
    std::lock_guard<std::mutex> lock(ctx->mtx);
    if (hdr->bytes == 0) return; // GPU surface handle not supported
    // Treat payload after hdr as AVPacket
    AVPacket pkt;
    av_init_packet(&pkt);
    pkt.data = (uint8_t*)frame + sizeof(zm_frame_hdr_t);
    pkt.size = (int)size - (int)sizeof(zm_frame_hdr_t);
    int ret = avcodec_send_packet(ctx->codec_ctx, &pkt);
    if (ret < 0) return;
    AVFrame* avf = av_frame_alloc();
    while (avcodec_receive_frame(ctx->codec_ctx, avf) == 0) {
        int w = avf->width, h = avf->height;
        AVPixelFormat pix_fmt = (AVPixelFormat)avf->format;
        // Setup swscale if needed
        if (ctx->scale != "orig") {
            if (ctx->sws_ctx == nullptr || ctx->out_width != w || ctx->out_height != h) {
                if (ctx->sws_ctx) sws_freeContext(ctx->sws_ctx);
                if (ctx->scale == "720p") {
                    ctx->out_width = 1280; ctx->out_height = 720;
                } else if (ctx->scale.find('x') != std::string::npos) {
                    sscanf(ctx->scale.c_str(), "%dx%d", &ctx->out_width, &ctx->out_height);
                } else {
                    ctx->out_width = w; ctx->out_height = h;
                }
                ctx->sws_ctx = sws_getContext(w, h, pix_fmt, ctx->out_width, ctx->out_height, ctx->out_pix_fmt, SWS_BILINEAR, nullptr, nullptr, nullptr);
            }
        } else {
            ctx->out_width = w; ctx->out_height = h;
        }
        int yuv_size = av_image_get_buffer_size(ctx->out_pix_fmt, ctx->out_width, ctx->out_height, 1);
        ctx->yuv_buf.resize(yuv_size);
        uint8_t* dst[4] = { ctx->yuv_buf.data(), nullptr, nullptr, nullptr };
        int dst_linesize[4] = { ctx->out_width, 0, 0, 0 };
        if (ctx->sws_ctx) {
            sws_scale(ctx->sws_ctx, avf->data, avf->linesize, 0, h, dst, dst_linesize);
        } else {
            av_image_copy_plane(dst[0], ctx->out_width, avf->data[0], avf->linesize[0], ctx->out_width, ctx->out_height);
            // U and V planes follow Y in buffer
            memcpy(ctx->yuv_buf.data() + ctx->out_width * ctx->out_height, avf->data[1], (ctx->out_width/2)*(ctx->out_height/2));
            memcpy(ctx->yuv_buf.data() + ctx->out_width * ctx->out_height + (ctx->out_width/2)*(ctx->out_height/2), avf->data[2], (ctx->out_width/2)*(ctx->out_height/2));
        }
        // Allocate output buffer
        void* out_buf = malloc(ctx->yuv_buf.size());
        if (!out_buf) {
            log(ctx->host, ctx->host_ctx, 3, "decode_ffmpeg: frame dropped – no output buf");
            av_frame_free(&avf);
            return;
        }
        memcpy(out_buf, ctx->yuv_buf.data(), ctx->yuv_buf.size());
        zm_frame_hdr_t out_hdr = *hdr;
        out_hdr.hw_type = 0;
        out_hdr.bytes = ctx->yuv_buf.size();
        out_hdr.pts_usec = avf->best_effort_timestamp;
        if (ctx->host && ctx->host->on_frame)
            ctx->host->on_frame(ctx->host_ctx, &out_hdr, sizeof(zm_frame_hdr_t) + ctx->yuv_buf.size());
        free(out_buf);
    }
    av_frame_free(&avf);
}

extern "C" __attribute__((visibility("default"))) void zm_plugin_init(zm_plugin_t* plugin) {
    if (!plugin) return;
    plugin->version = 1;
    plugin->type = ZM_PLUGIN_PROCESS;
    plugin->instance = nullptr;
    plugin->start = process_start;
    plugin->stop = process_stop;
    // Adapter for correct function pointer signature
    plugin->on_frame = [](zm_plugin_t* p, const zm_frame_hdr_t* hdr, const void* frame) {
        // Assume frame points to a buffer that is at least hdr->bytes in size after the header
        // We don't know the real size, so pass a guessed size (hdr->bytes + sizeof(zm_frame_hdr_t))
        process_on_frame(p, hdr, frame, hdr ? hdr->bytes + sizeof(zm_frame_hdr_t) : 0);
    };
}
