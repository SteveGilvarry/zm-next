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
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <libavutil/base64.h>
}

// Map a config "hwaccel" string to an FFmpeg hw device type. "none"/"auto"->NONE
// ("auto" is resolved later by probing). Unknown strings -> NONE.
static enum AVHWDeviceType hwaccel_device_type(const std::string& s) {
    if (s == "cuda")          return AV_HWDEVICE_TYPE_CUDA;
    if (s == "videotoolbox")  return AV_HWDEVICE_TYPE_VIDEOTOOLBOX;
    if (s == "vaapi")         return AV_HWDEVICE_TYPE_VAAPI;
    if (s == "qsv")           return AV_HWDEVICE_TYPE_QSV;
    if (s == "d3d11va")       return AV_HWDEVICE_TYPE_D3D11VA;
    if (s == "dxva2")         return AV_HWDEVICE_TYPE_DXVA2;
    return AV_HWDEVICE_TYPE_NONE;
}

// The decoded-surface pixel format produced by each hw device type.
static enum AVPixelFormat hw_device_pix_fmt(enum AVHWDeviceType t) {
    switch (t) {
        case AV_HWDEVICE_TYPE_CUDA:         return AV_PIX_FMT_CUDA;
        case AV_HWDEVICE_TYPE_VIDEOTOOLBOX: return AV_PIX_FMT_VIDEOTOOLBOX;
        case AV_HWDEVICE_TYPE_VAAPI:        return AV_PIX_FMT_VAAPI;
        case AV_HWDEVICE_TYPE_QSV:          return AV_PIX_FMT_QSV;
        case AV_HWDEVICE_TYPE_D3D11VA:      return AV_PIX_FMT_D3D11;
        case AV_HWDEVICE_TYPE_DXVA2:        return AV_PIX_FMT_DXVA2_VLD;
        default:                            return AV_PIX_FMT_NONE;
    }
}

// Resolve the input codec id from a config "codec" string (with common aliases).
static enum AVCodecID decoder_id_from_name(const std::string& s) {
    if (s == "h264" || s == "avc")            return AV_CODEC_ID_H264;
    if (s == "h265" || s == "hevc")           return AV_CODEC_ID_HEVC;
    if (s == "mjpeg")                         return AV_CODEC_ID_MJPEG;
    if (s == "mpeg4")                         return AV_CODEC_ID_MPEG4;
    if (s == "av1")                           return AV_CODEC_ID_AV1;
    if (s == "vp8")                           return AV_CODEC_ID_VP8;
    if (s == "vp9")                           return AV_CODEC_ID_VP9;
    return AV_CODEC_ID_NONE;
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
    std::string codec_name = "h264";     // input codec fallback (h264/hevc/...)
    bool auto_codec = true;              // auto-detect input codec from StreamMetadata
    bool decoder_ready = false;          // decoder created lazily on first frame
    struct DecodeMeta* meta = nullptr;   // leaked auto-detect state (callback-shared)
    void* meta_sub = nullptr;            // host event subscription handle
    bool hw_decode = false;
    std::string hwaccel = "none";        // none|auto|cuda|videotoolbox|vaapi|qsv|...
    enum AVHWDeviceType hw_type = AV_HWDEVICE_TYPE_NONE;
    enum AVPixelFormat hw_pix_fmt = AV_PIX_FMT_NONE;  // surface fmt for hw_type
    bool use_cuda = false;               // CUDA zero-copy surface path active
    AVBufferRef* hw_device_ctx = nullptr;
    AVCodecContext* codec_ctx = nullptr;
    SwsContext* sws_ctx = nullptr;
    AVFrame* sw_frame = nullptr;         // for downloading non-CUDA hw frames
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
        if (sw_frame) av_frame_free(&sw_frame);
        if (hw_device_ctx) av_buffer_unref(&hw_device_ctx);
    }
};

// get_format callback: pick the hw surface format for the configured device.
// Reads the desired format from AVCodecContext::opaque (the DecoderCtx).
static enum AVPixelFormat decode_get_hw_format(AVCodecContext* c,
                                               const enum AVPixelFormat* fmts) {
    auto* ctx = static_cast<DecoderCtx*>(c->opaque);
    const enum AVPixelFormat want = ctx ? ctx->hw_pix_fmt : AV_PIX_FMT_NONE;
    for (const enum AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; ++p)
        if (*p == want) return *p;
    return AV_PIX_FMT_NONE;
}

static void log(zm_host_api_t* host, void* ctx, int lvl, const std::string& msg) {
    if (host && host->log) host->log(ctx, (zm_log_level_t)lvl, msg.c_str());
}

// Auto-detect state shared with the host event callback. Leaked on stop so an
// in-flight callback is always safe (codec_id is a single early write).
struct DecodeMeta {
    std::atomic<int> codec_id{(int)AV_CODEC_ID_NONE};
    std::atomic<bool> running{true};
    std::mutex mtx;
    std::vector<uint8_t> extradata;   // codec extradata (AVCC/HVCC SPS/PPS), guarded by mtx
};

// Host event callback: learn the input codec id (and extradata) from the capture
// plugin's video StreamMetadata event, so decode auto-detects H264/HEVC/etc. The
// extradata matters for AVCC/HVCC sources (e.g. MP4 files) where SPS/PPS live out
// of band and the raw packets are length-prefixed, not Annex-B; without it the
// decoder can't parse the bitstream ("no start code found").
static void decode_meta_cb(void* user, const char* json_event) {
    auto* meta = static_cast<DecodeMeta*>(user);
    if (!meta || !meta->running.load() || !json_event) return;
    try {
        auto j = nlohmann::json::parse(json_event);
        if (j.value("event", std::string()) != "StreamMetadata") return;
        if (j.value("media", std::string("video")) != "video") return;
        meta->codec_id.store(j.value("codec_id", (int)AV_CODEC_ID_NONE));
        std::string ed_b64 = j.value("extradata", std::string());
        if (!ed_b64.empty()) {
            size_t max_out = (ed_b64.size() * 3) / 4 + 1;
            std::vector<uint8_t> tmp(max_out);
            int n = av_base64_decode(tmp.data(), ed_b64.c_str(), (int)max_out);
            if (n > 0) {
                tmp.resize(n);
                std::lock_guard<std::mutex> lk(meta->mtx);
                meta->extradata = std::move(tmp);
            }
        }
    } catch (...) {
        // ignore malformed events
    }
}

// Lazily create the decoder on the first frame, choosing the codec from the
// auto-detected StreamMetadata id when available, else the configured fallback.
static bool ensure_decoder(DecoderCtx* ctx) {
    if (ctx->decoder_ready) return true;

    const AVCodec* codec = nullptr;
    if (ctx->auto_codec && ctx->meta) {
        auto cid = (enum AVCodecID)ctx->meta->codec_id.load();
        if (cid != AV_CODEC_ID_NONE) codec = avcodec_find_decoder(cid);
    }
    if (!codec) {  // fallback to the configured codec name (default "h264")
        enum AVCodecID ncid = decoder_id_from_name(ctx->codec_name);
        codec = (ncid != AV_CODEC_ID_NONE) ? avcodec_find_decoder(ncid)
                                           : avcodec_find_decoder_by_name(ctx->codec_name.c_str());
    }
    if (!codec) return false;  // no metadata yet and no usable fallback; wait

    ctx->codec_ctx = avcodec_alloc_context3(codec);
    if (!ctx->codec_ctx) { log(ctx->host, ctx->host_ctx, 3, "decode_ffmpeg: alloc ctx failed"); return false; }
    ctx->codec_ctx->thread_count = ctx->threads;
    ctx->codec_ctx->thread_type = ctx->threads > 0 ? FF_THREAD_FRAME : 0;
    ctx->codec_ctx->opaque = ctx;  // for the get_format callback

    // Apply extradata (SPS/PPS) from StreamMetadata. Required for AVCC/HVCC
    // sources (length-prefixed NALs, out-of-band parameter sets); harmless for
    // Annex-B sources (RTSP) which simply carry no extradata and parse in-band.
    if (ctx->meta) {
        std::vector<uint8_t> ed;
        {
            std::lock_guard<std::mutex> lk(ctx->meta->mtx);
            ed = ctx->meta->extradata;
        }
        if (!ed.empty()) {
            ctx->codec_ctx->extradata =
                (uint8_t*)av_mallocz(ed.size() + AV_INPUT_BUFFER_PADDING_SIZE);
            if (ctx->codec_ctx->extradata) {
                std::memcpy(ctx->codec_ctx->extradata, ed.data(), ed.size());
                ctx->codec_ctx->extradata_size = (int)ed.size();
            }
        }
    }

    // Optional hardware decode (CUDA = zero-copy surface; others download to CPU).
    enum AVHWDeviceType want = (ctx->hwaccel == "auto")
                                   ? (
#ifdef __APPLE__
                                      AV_HWDEVICE_TYPE_VIDEOTOOLBOX
#else
                                      AV_HWDEVICE_TYPE_CUDA
#endif
                                     )
                                   : hwaccel_device_type(ctx->hwaccel);
    if (want != AV_HWDEVICE_TYPE_NONE) {
        int herr = av_hwdevice_ctx_create(&ctx->hw_device_ctx, want, nullptr, nullptr, 0);
        if (herr < 0 || !ctx->hw_device_ctx) {
            log(ctx->host, ctx->host_ctx, 2, "decode_ffmpeg: hwaccel '" + ctx->hwaccel +
                "' unavailable (" + get_av_error_string(herr) + "); using software decode");
            if (ctx->hw_device_ctx) av_buffer_unref(&ctx->hw_device_ctx);
        } else {
            ctx->hw_type = want;
            ctx->hw_pix_fmt = hw_device_pix_fmt(want);
            ctx->use_cuda = (want == AV_HWDEVICE_TYPE_CUDA);
            ctx->codec_ctx->hw_device_ctx = av_buffer_ref(ctx->hw_device_ctx);
            ctx->codec_ctx->get_format = decode_get_hw_format;
            log(ctx->host, ctx->host_ctx, 4, "decode_ffmpeg: hardware decode (" + ctx->hwaccel + ")");
        }
    }

    if (avcodec_open2(ctx->codec_ctx, codec, nullptr) < 0) {
        log(ctx->host, ctx->host_ctx, 3, "decode_ffmpeg: failed to open codec");
        avcodec_free_context(&ctx->codec_ctx);
        return false;
    }
    ctx->decoder_ready = true;
    log(ctx->host, ctx->host_ctx, 4, std::string("decode_ffmpeg: decoder ready codec=") +
        (codec->name ? codec->name : "?") + " (" + (ctx->auto_codec ? "auto" : "configured") +
        "), output_format=" + ctx->output_format + ", hwaccel=" + ctx->hwaccel);
    // Stop listening for metadata once the decoder exists.
    if (ctx->host && ctx->host->unsubscribe_evt && ctx->meta_sub) {
        ctx->host->unsubscribe_evt(ctx->host_ctx, ctx->meta_sub);
        ctx->meta_sub = nullptr;
        if (ctx->meta) ctx->meta->running.store(false);
    }
    return true;
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
        // "codec" is an OPTIONAL override; without it the input codec is
        // auto-detected from the capture plugin's StreamMetadata.
        if (cfg.contains("codec")) {
            ctx->codec_name = cfg["codec"].get<std::string>();
            ctx->auto_codec = false;
        }
        ctx->hw_decode = cfg.value("hw_decode", false);
        ctx->hwaccel = cfg.value("hwaccel", std::string("none"));

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

    // Subscribe (via the host) to learn the input codec from StreamMetadata. The
    // decoder is created lazily on the first frame (by then metadata has arrived).
    ctx->meta = new DecodeMeta();
    if (host && host->subscribe_evt)
        ctx->meta_sub = host->subscribe_evt(host_ctx, &decode_meta_cb, ctx->meta);

    log(host, host_ctx, 4, std::string("decode_ffmpeg: started (codec=") +
        (ctx->auto_codec ? "auto" : ctx->codec_name) + ")");
    plugin->instance = ctx;
    return 0;
}

static void process_stop(zm_plugin_t* plugin) {
    if (!plugin || !plugin->instance) return;
    auto ctx = static_cast<DecoderCtx*>(plugin->instance);
    ctx->running = false;
    // Stop metadata deliveries; the DecodeMeta is intentionally leaked so an
    // in-flight callback can't dereference freed memory.
    if (ctx->host && ctx->host->unsubscribe_evt && ctx->meta_sub)
        ctx->host->unsubscribe_evt(ctx->host_ctx, ctx->meta_sub);
    if (ctx->meta) ctx->meta->running.store(false);
    delete ctx;
    plugin->instance = nullptr;
}


// Standardized single-buffer on_frame: buf = [zm_frame_hdr_t][payload]
static void process_on_frame(zm_plugin_t* plugin, const void* buf, size_t size) {
    if (!plugin || !plugin->instance || !buf || size < sizeof(zm_frame_hdr_t)) return;
    auto ctx = static_cast<DecoderCtx*>(plugin->instance);
    std::lock_guard<std::mutex> lock(ctx->mtx);
    const zm_frame_hdr_t* hdr = (const zm_frame_hdr_t*)buf;
    // Only decode compressed video. Anything else (compressed audio, already-
    // decoded frames, GPU surfaces) is forwarded downstream untouched so audio
    // and other consumers still receive it.
    if (hdr->hw_type != ZM_FRAME_COMPRESSED) {
        if (ctx->host && ctx->host->on_frame) ctx->host->on_frame(ctx->host_ctx, buf, size);
        return;
    }
    // Create the decoder on first use (codec auto-detected from StreamMetadata,
    // else the configured fallback). Skip the frame until a decoder can be made.
    if (!ensure_decoder(ctx)) return;
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
    // Carry the source timestamp (microseconds) into the packet so the decoder
    // returns it as best_effort_timestamp on the decoded frame; otherwise every
    // downstream consumer (motion, detect, store) sees AV_NOPTS_VALUE.
    pkt->pts = pkt->dts = static_cast<int64_t>(hdr->pts_usec);

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

        // Zero-copy GPU path: the frame is a CUDA surface. Emit a descriptor
        // referencing the device memory instead of downloading/converting on CPU.
        // The AVFrame stays valid for the synchronous downstream on_frame call.
        if (avf->format == AV_PIX_FMT_CUDA && avf->hw_frames_ctx) {
            zm_gpu_frame_t g{};
            g.hw_type = ZM_HW_CUDA;
            g.width = avf->width;
            g.height = avf->height;
            auto* fctx = reinterpret_cast<AVHWFramesContext*>(avf->hw_frames_ctx->data);
            g.pix_fmt = static_cast<uint32_t>(fctx ? fctx->sw_format : AV_PIX_FMT_NV12);
            for (int i = 0; i < 4; ++i) {
                g.plane_ptr[i] = reinterpret_cast<uint64_t>(avf->data[i]);
                g.linesize[i] = static_cast<uint32_t>(avf->linesize[i]);
            }
            g.av_frame = reinterpret_cast<uint64_t>(avf);
            g.device_ctx = reinterpret_cast<uint64_t>(ctx->codec_ctx->hw_device_ctx);

            std::vector<uint8_t> out_buf(sizeof(zm_frame_hdr_t) + sizeof(zm_gpu_frame_t));
            zm_frame_hdr_t out_hdr = *hdr;
            out_hdr.hw_type = ZM_HW_CUDA;
            out_hdr.bytes = sizeof(zm_gpu_frame_t);
            out_hdr.pts_usec = avf->best_effort_timestamp;
            memcpy(out_buf.data(), &out_hdr, sizeof(zm_frame_hdr_t));
            memcpy(out_buf.data() + sizeof(zm_frame_hdr_t), &g, sizeof(zm_gpu_frame_t));

            if (ctx->host && ctx->host->on_frame)
                ctx->host->on_frame(ctx->host_ctx, out_buf.data(), out_buf.size());
            av_frame_unref(avf);
            continue;  // next frame; skip the CPU swscale path below
        }

        // Non-CUDA hardware frame (VideoToolbox/VAAPI/QSV): download to CPU and
        // continue through the normal swscale path (no zero-copy consumer exists
        // for those surfaces today). Software frames skip this.
        if (avf->hw_frames_ctx && avf->format == ctx->hw_pix_fmt) {
            if (!ctx->sw_frame) ctx->sw_frame = av_frame_alloc();
            const int64_t pts = avf->best_effort_timestamp;
            if (av_hwframe_transfer_data(ctx->sw_frame, avf, 0) < 0) {
                log(ctx->host, ctx->host_ctx, 3, "decode_ffmpeg: hw frame download failed");
                av_frame_unref(avf);
                continue;
            }
            av_frame_unref(avf);
            av_frame_move_ref(avf, ctx->sw_frame);  // avf now holds the CPU frame
            avf->best_effort_timestamp = pts;
        }

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
