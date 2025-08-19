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
#include <libavutil/error.h>
}

// Helper function for error strings
static std::string get_av_error_string(int errnum) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(errnum, errbuf, AV_ERROR_MAX_STRING_SIZE);
    return std::string(errbuf);
}

struct DecoderCtx {
    int threads = 0;
    std::string scale = "orig";
    std::string output_format = "yuv420p";
    bool hw_decode = false;
    AVCodecContext* codec_ctx = nullptr;
    SwsContext* sws_ctx = nullptr;
    int out_width = 0, out_height = 0;
    AVPixelFormat out_pix_fmt = AV_PIX_FMT_YUV420P;
    std::vector<uint8_t> frame_buf;
    zm_host_api_t* host = nullptr;
    void* host_ctx = nullptr;
    std::mutex mtx;
    std::atomic<bool> running{true};
    int decode_errors = 0;
    int frames_decoded = 0;
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
        ctx->output_format = cfg.value("output_format", "yuv420p");
        ctx->hw_decode = cfg.value("hw_decode", false);
        
        // Set output pixel format based on configuration
        if (ctx->output_format == "rgb24") {
            ctx->out_pix_fmt = AV_PIX_FMT_RGB24;
        } else if (ctx->output_format == "gray" || ctx->output_format == "gray8") {
            ctx->out_pix_fmt = AV_PIX_FMT_GRAY8;
        } else {
            ctx->out_pix_fmt = AV_PIX_FMT_YUV420P;
        }
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
    oss << "decode_ffmpeg: created decoder H264, output_format=" << ctx->output_format 
        << ", size=" << ctx->codec_ctx->width << "x" << ctx->codec_ctx->height;
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


// Standardized single-buffer on_frame: buf = [zm_frame_hdr_t][payload]
static void process_on_frame(zm_plugin_t* plugin, const void* buf, size_t size) {
    if (!plugin || !plugin->instance || !buf || size < sizeof(zm_frame_hdr_t)) return;
    auto ctx = static_cast<DecoderCtx*>(plugin->instance);
    std::lock_guard<std::mutex> lock(ctx->mtx);
    const zm_frame_hdr_t* hdr = (const zm_frame_hdr_t*)buf;
    if (hdr->bytes == 0) return; // GPU surface handle not supported
    if (size < sizeof(zm_frame_hdr_t) + hdr->bytes) return;
    
    const uint8_t* payload = (const uint8_t*)buf + sizeof(zm_frame_hdr_t);
    
    AVPacket* pkt = av_packet_alloc();
    if (!pkt) {
        log(ctx->host, ctx->host_ctx, 3, "decode_ffmpeg: failed to allocate packet");
        return;
    }
    
    pkt->data = (uint8_t*)payload;
    pkt->size = hdr->bytes;
    
    int ret = avcodec_send_packet(ctx->codec_ctx, pkt);
    if (ret < 0) {
        ctx->decode_errors++;
        if (ctx->decode_errors % 10 == 1) { // Log every 10th error
            std::ostringstream oss;
            oss << "decode_ffmpeg: send_packet failed: " << get_av_error_string(ret) 
                << " (error #" << ctx->decode_errors << ")";
            log(ctx->host, ctx->host_ctx, 3, oss.str());
        }
        av_packet_free(&pkt);
        return;
    }
    
    av_packet_free(&pkt);
    
    AVFrame* avf = av_frame_alloc();
    if (!avf) {
        log(ctx->host, ctx->host_ctx, 3, "decode_ffmpeg: failed to allocate frame");
        return;
    }
    
    while (avcodec_receive_frame(ctx->codec_ctx, avf) == 0) {
        ctx->frames_decoded++;
        
        int w = avf->width, h = avf->height;
        AVPixelFormat src_pix_fmt = (AVPixelFormat)avf->format;
        
        // Log frame info occasionally
        if (ctx->frames_decoded % 100 == 1) {
            std::ostringstream oss;
            oss << "decode_ffmpeg: decoded frame #" << ctx->frames_decoded 
                << " size=" << w << "x" << h << " fmt=" << av_get_pix_fmt_name(src_pix_fmt);
            log(ctx->host, ctx->host_ctx, 4, oss.str());
        }
        
        // Setup output dimensions
        if (ctx->scale != "orig") {
            if (ctx->scale == "720p") {
                ctx->out_width = 1280; 
                ctx->out_height = 720;
            } else if (ctx->scale.find('x') != std::string::npos) {
                sscanf(ctx->scale.c_str(), "%dx%d", &ctx->out_width, &ctx->out_height);
            } else {
                ctx->out_width = w; 
                ctx->out_height = h;
            }
        } else {
            ctx->out_width = w; 
            ctx->out_height = h;
        }
        
        // Setup swscale context if needed (format conversion or scaling)
        bool needs_conversion = (src_pix_fmt != ctx->out_pix_fmt) || 
                               (ctx->out_width != w) || (ctx->out_height != h);
        
        if (needs_conversion) {
            if (ctx->sws_ctx == nullptr || 
                ctx->out_width != w || ctx->out_height != h) {
                if (ctx->sws_ctx) sws_freeContext(ctx->sws_ctx);
                
                ctx->sws_ctx = sws_getContext(
                    w, h, src_pix_fmt,
                    ctx->out_width, ctx->out_height, ctx->out_pix_fmt,
                    SWS_BILINEAR, nullptr, nullptr, nullptr);
                
                if (!ctx->sws_ctx) {
                    log(ctx->host, ctx->host_ctx, 3, "decode_ffmpeg: failed to create swscale context");
                    av_frame_free(&avf);
                    return;
                }
                
                std::ostringstream oss;
                oss << "decode_ffmpeg: created swscale " << w << "x" << h << " " 
                    << av_get_pix_fmt_name(src_pix_fmt) << " -> " 
                    << ctx->out_width << "x" << ctx->out_height << " " 
                    << av_get_pix_fmt_name(ctx->out_pix_fmt);
                log(ctx->host, ctx->host_ctx, 4, oss.str());
            }
        }
        
        // Calculate output buffer size
        int out_size = av_image_get_buffer_size(ctx->out_pix_fmt, ctx->out_width, ctx->out_height, 1);
        if (out_size < 0) {
            log(ctx->host, ctx->host_ctx, 3, "decode_ffmpeg: failed to calculate output buffer size");
            av_frame_free(&avf);
            return;
        }
        
        ctx->frame_buf.resize(out_size);
        
        if (needs_conversion && ctx->sws_ctx) {
            // Use swscale for format conversion/scaling
            uint8_t* dst_data[4];
            int dst_linesize[4];
            
            ret = av_image_fill_arrays(dst_data, dst_linesize, ctx->frame_buf.data(), 
                                     ctx->out_pix_fmt, ctx->out_width, ctx->out_height, 1);
            if (ret < 0) {
                log(ctx->host, ctx->host_ctx, 3, "decode_ffmpeg: failed to setup output arrays");
                av_frame_free(&avf);
                return;
            }
            
            ret = sws_scale(ctx->sws_ctx, avf->data, avf->linesize, 0, h, dst_data, dst_linesize);
            if (ret < 0) {
                log(ctx->host, ctx->host_ctx, 3, "decode_ffmpeg: swscale failed");
                av_frame_free(&avf);
                return;
            }
        } else {
            // Direct copy when no conversion needed
            av_image_copy_to_buffer(ctx->frame_buf.data(), out_size, 
                                  (const uint8_t**)avf->data, avf->linesize,
                                  ctx->out_pix_fmt, ctx->out_width, ctx->out_height, 1);
        }
        
        // Allocate output buffer with header
        std::vector<uint8_t> out_buf(sizeof(zm_frame_hdr_t) + ctx->frame_buf.size());
        zm_frame_hdr_t out_hdr = *hdr;
        
        // Set frame format based on output pixel format
        if (ctx->out_pix_fmt == AV_PIX_FMT_RGB24) {
            out_hdr.hw_type = ZM_FRAME_RGB24;
        } else if (ctx->out_pix_fmt == AV_PIX_FMT_GRAY8) {
            out_hdr.hw_type = ZM_FRAME_GRAYSCALE;
        } else {
            out_hdr.hw_type = ZM_FRAME_YUV420P;
        }
        
        out_hdr.bytes = ctx->frame_buf.size();
        out_hdr.pts_usec = avf->best_effort_timestamp;
        
        memcpy(out_buf.data(), &out_hdr, sizeof(zm_frame_hdr_t));
        memcpy(out_buf.data() + sizeof(zm_frame_hdr_t), ctx->frame_buf.data(), ctx->frame_buf.size());
        
        // Send frame to next plugin
        if (ctx->host && ctx->host->on_frame) {
            ctx->host->on_frame(ctx->host_ctx, out_buf.data(), out_buf.size());
        }
        
        // Log successful frame processing occasionally
        if (ctx->frames_decoded % 100 == 0) {
            std::ostringstream oss;
            oss << "decode_ffmpeg: processed " << ctx->frames_decoded << " frames, " 
                << ctx->decode_errors << " errors, output=" << ctx->frame_buf.size() << " bytes";
            log(ctx->host, ctx->host_ctx, 4, oss.str());
        }
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
    plugin->on_frame = process_on_frame;
}
