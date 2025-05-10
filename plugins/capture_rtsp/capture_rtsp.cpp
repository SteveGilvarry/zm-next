#include "zm_plugin.h"
// Ensure C linkage for FFmpeg headers
#ifdef __cplusplus
extern "C" {
#endif
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>  // For sws_getContext
#ifdef __cplusplus
}
#endif
#include <cstring>
#include <cstdlib>
#include <thread>
#include <atomic>

// Plugin-specific context holding FFmpeg and thread info
struct Context {
    AVFormatContext* fmtCtx = nullptr;
    AVCodecContext* decCtx = nullptr;
    int streamIndex = -1;
    std::thread worker;
    std::atomic<bool> running{false};
    const char* url = nullptr;
    // Temporary buffer for frame conversion
    uint8_t* frameBuffer = nullptr;
    size_t frameBufferSize = 0;
    // Frame counter for debug
    int frameCount = 0;
    // Frame conversion context
    SwsContext* swsCtx = nullptr;
    uint8_t* rgbFrameData[4] = {nullptr};
    int rgbLinesize[4] = {0};
};

extern "C" {

// Forward-declare the frame callback function
typedef void (*frame_callback_t)(const zm_frame_hdr_t* hdr, const void* payload, size_t payload_size);

// Global callback state
static frame_callback_t g_frame_callback = nullptr;
static void* g_callback_ctx = nullptr;

// Function to register a callback for pushing frames
void register_frame_callback(zm_plugin_t* plugin,
                          frame_callback_t callback,
                          void* callback_ctx);

// Forward declarations
static void start_plugin(zm_plugin_t *plugin);
static void stop_plugin(zm_plugin_t *plugin);
static void on_frame(zm_plugin_t *plugin, const zm_frame_hdr_t *hdr, const void *payload, size_t payload_size);

void init_plugin(zm_plugin_t *plugin) {
    // Initialize FFmpeg network components
    avformat_network_init();

    // Allocate and initialize plugin context
    Context* ctx = new Context();
    // Default RTSP URL (override via pipeline config if needed)
    ctx->url = "rtsp://127.0.0.1:8554/stream";

    plugin->type = ZM_PLUGIN_INPUT;
    plugin->instance = ctx;
    plugin->start = start_plugin;
    plugin->stop = stop_plugin;
    // Input plugin frames are consumed by CaptureThread
    plugin->on_frame = nullptr;
}

void start_plugin(zm_plugin_t *plugin) {
    Context* ctx = static_cast<Context*>(plugin->instance);
    if (!ctx) return;
    ctx->running = true;
    // Launch worker thread for RTSP capture
    ctx->worker = std::thread([ctx, plugin]() {
        // Open RTSP input
        AVDictionary* options = nullptr;
        // Set RTSP options for low latency
        av_dict_set(&options, "rtsp_transport", "tcp", 0);      // Use TCP for more reliability
        av_dict_set(&options, "max_delay", "500000", 0);        // 500ms max delay
        av_dict_set(&options, "fflags", "nobuffer", 0);         // Don't buffer frames
        av_dict_set(&options, "stimeout", "5000000", 0);        // Socket timeout in microseconds (5s)
        av_dict_set(&options, "reconnect", "1", 0);             // Auto reconnect
        av_dict_set(&options, "reconnect_streamed", "1", 0);    // Auto reconnect for streamed media
        av_dict_set(&options, "reconnect_delay_max", "5", 0);   // Max 5 seconds between reconnection attempts
        
        if (avformat_open_input(&ctx->fmtCtx, ctx->url, nullptr, &options) < 0) {
            host_log("capture_rtsp: failed to open input");
            av_dict_free(&options);
            return;
        }
        av_dict_free(&options);

        if (avformat_find_stream_info(ctx->fmtCtx, nullptr) < 0) {
            host_log("capture_rtsp: failed to find stream info");
            return;
        }
        // Locate video stream
        ctx->streamIndex = -1;
        for (unsigned i = 0; i < ctx->fmtCtx->nb_streams; ++i) {
            if (ctx->fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                ctx->streamIndex = i;
                break;
            }
        }
        
        if (ctx->streamIndex == -1) {
            host_log("capture_rtsp: no video stream found");
            return;
        }
        
        // Find decoder for the stream
        AVStream* stream = ctx->fmtCtx->streams[ctx->streamIndex];
        const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
        if (!codec) {
            host_log("capture_rtsp: unsupported codec");
            return;
        }
        
        // Allocate codec context
        ctx->decCtx = avcodec_alloc_context3(codec);
        if (!ctx->decCtx) {
            host_log("capture_rtsp: could not allocate codec context");
            return;
        }
        
        // Copy codec parameters to context
        if (avcodec_parameters_to_context(ctx->decCtx, stream->codecpar) < 0) {
            host_log("capture_rtsp: failed to copy codec parameters");
            return;
        }
        
        // Open the codec
        if (avcodec_open2(ctx->decCtx, codec, nullptr) < 0) {
            host_log("capture_rtsp: failed to open codec");
            return;
        }
        
        // Allocate frame buffer and setup scaling context
        int width = ctx->decCtx->width;
        int height = ctx->decCtx->height;
        int bufferSize = av_image_get_buffer_size(AV_PIX_FMT_RGB24, width, height, 1);
        ctx->frameBufferSize = bufferSize;
        ctx->frameBuffer = static_cast<uint8_t*>(av_malloc(bufferSize));
        if (!ctx->frameBuffer) {
            host_log("capture_rtsp: failed to allocate frame buffer");
            return;
        }
        
        // Initialize SwsContext for YUV to RGB conversion
        ctx->swsCtx = sws_getContext(width, height, ctx->decCtx->pix_fmt,
                                    width, height, AV_PIX_FMT_RGB24,
                                    SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!ctx->swsCtx) {
            host_log("capture_rtsp: failed to create scaling context");
            return;
        }
        
        // Setup RGB frame data structure
        av_image_fill_arrays(ctx->rgbFrameData, ctx->rgbLinesize, 
                            ctx->frameBuffer, AV_PIX_FMT_RGB24, 
                            width, height, 1);
        
        char logMsg[128];
        snprintf(logMsg, sizeof(logMsg), "capture_rtsp: opened video stream %dx%d, codec %s", 
                 width, height, codec->name);
        host_log(logMsg);
        
        // Initialize frame counter
        ctx->frameCount = 0;
        
        // Allocate packet and frame
        AVPacket* packet = av_packet_alloc();
        if (!packet) {
            host_log("capture_rtsp: failed to allocate packet");
            return;
        }
        
        AVFrame* frame = av_frame_alloc();
        if (!frame) {
            host_log("capture_rtsp: failed to allocate frame");
            av_packet_free(&packet);
            return;
        }
        
        // Main decoding loop
        while (ctx->running && av_read_frame(ctx->fmtCtx, packet) >= 0) {
            if (packet->stream_index == ctx->streamIndex) {
                // Send packet to decoder
                int ret = avcodec_send_packet(ctx->decCtx, packet);
                if (ret < 0) {
                    host_log("capture_rtsp: error sending packet to decoder");
                } else {
                    // Process all decoded frames
                    while (ret >= 0) {
                        ret = avcodec_receive_frame(ctx->decCtx, frame);
                        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                            break;
                        } else if (ret < 0) {
                            host_log("capture_rtsp: error during decoding");
                            break;
                        }
                        
                        // We have a frame, pack it for sending
                        ctx->frameCount++;
                        
                        // If we have a push_frame callback, use it
                        if (g_frame_callback) {
                            // Convert the frame from its native format to RGB24
                            sws_scale(ctx->swsCtx, 
                                    (const uint8_t* const*)frame->data, frame->linesize,
                                    0, frame->height, 
                                    ctx->rgbFrameData, ctx->rgbLinesize);
                            
                            // Create frame header
                            zm_frame_hdr_t hdr;
                            hdr.width = frame->width;
                            hdr.height = frame->height;
                            hdr.timestamp = frame->pts != AV_NOPTS_VALUE ? frame->pts : 
                                          (frame->pkt_dts != AV_NOPTS_VALUE ? frame->pkt_dts : 0);
                            hdr.hw_type = (uint32_t)reinterpret_cast<uintptr_t>(g_callback_ctx); // Pass the context in header
                            
                            // The RGB data is now in ctx->frameBuffer
                            size_t rgbDataSize = frame->width * frame->height * 3; // RGB24 = 3 bytes per pixel
                            
                            // Call host to push frame
                            g_frame_callback(&hdr, ctx->frameBuffer, rgbDataSize);
                            
                            // Log progress occasionally
                            if (ctx->frameCount % 100 == 0) {
                                char buffer[64];
                                snprintf(buffer, sizeof(buffer), "capture_rtsp: captured %d frames", ctx->frameCount);
                                host_log(buffer);
                            }
                        }
                    }
                }
            }
            av_packet_unref(packet);
        }
        
        // Clean up
        av_frame_free(&frame);
        av_packet_free(&packet);
        
        if (ctx->swsCtx) {
            sws_freeContext(ctx->swsCtx);
            ctx->swsCtx = nullptr;
        }
        
        if (ctx->frameBuffer) {
            av_free(ctx->frameBuffer);
            ctx->frameBuffer = nullptr;
        }
        
        avformat_close_input(&ctx->fmtCtx);
        
        host_log("capture_rtsp: capture thread finished");
    });
    host_log("capture_rtsp: capture thread started");
}

void stop_plugin(zm_plugin_t *plugin) {
    Context* ctx = static_cast<Context*>(plugin->instance);
    if (!ctx) return;
    // Signal thread to stop
    ctx->running = false;
    if (ctx->worker.joinable()) ctx->worker.join();
    host_log("capture_rtsp: capture thread stopped");
    
    // Free codec context if it exists
    if (ctx->decCtx) {
        avcodec_free_context(&ctx->decCtx);
    }
    
    // Free swscale context if it exists
    if (ctx->swsCtx) {
        sws_freeContext(ctx->swsCtx);
        ctx->swsCtx = nullptr;
    }
    
    // Free frame buffer if it exists
    if (ctx->frameBuffer) {
        av_free(ctx->frameBuffer);
        ctx->frameBuffer = nullptr;
    }
    
    // Cleanup FFmpeg network
    avformat_network_deinit();
    // Delete context
    delete ctx;
    plugin->instance = nullptr;
}

// Register a callback to receive frames
// This should be called by the host to establish frame handling
void register_frame_callback(zm_plugin_t* plugin, 
                           frame_callback_t push_frame,
                           void* callback_ctx) {
    if (!plugin || !push_frame) return;
    
    // Store callback in global variables
    g_frame_callback = push_frame;
    g_callback_ctx = callback_ctx;
    host_log("capture_rtsp: frame callback registered");
}

void on_frame(zm_plugin_t *plugin, const zm_frame_hdr_t *hdr, const void *payload, size_t payload_size) {
    // Not used in input plugin; frames are pulled by CaptureThread directly from the ring
    host_log("capture_rtsp: on_frame placeholder");
}

}
