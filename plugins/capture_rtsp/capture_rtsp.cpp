// Export both zm_plugin_init and init_plugin for compatibility

#include "zm_plugin.h"

// Ensure C linkage for FFmpeg headers
#ifdef __cplusplus
extern "C" {
#endif
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/dict.h>
#include <libavutil/time.h>
#include <libavutil/base64.h>    // for av_base64_encode
#ifdef __cplusplus
}
#endif

#include <thread>
#include <atomic>
#include <chrono>
#include <string>
#include <mutex>
#include <random>
#include <vector>

// Forward declaration of the JSON parser function
static bool parse_json_config(const char* json_cfg, std::string& url, std::string& transport,
                             int& max_streams, bool& hw_decode, zm_host_api_t* host, void* host_ctx);

// Plugin-specific context holding FFmpeg and thread info
struct RtspContext {
    // Configuration
    std::string url;
    std::string transport = "tcp";
    int max_streams = 2;      // Max number of streams to process
    bool hw_decode = true;    // Try hardware acceleration if available
    
    // FFmpeg contexts
    AVFormatContext* fmt_ctx = nullptr;
    AVPacket* packet = nullptr;
    AVBufferRef* hw_device_ctx = nullptr;
    
    // Stream information
    struct StreamInfo {
        int index = -1;        // Stream index in the format context
        AVCodecContext* codec_ctx = nullptr;
        AVMediaType type = AVMEDIA_TYPE_UNKNOWN;
        bool is_hw_accelerated = false;
        AVHWDeviceType hw_type = AV_HWDEVICE_TYPE_NONE;
    };
    std::vector<StreamInfo> streams;
    
    // Threading
    std::thread worker;
    std::atomic<bool> running{false};
    
    // Host API reference
    zm_host_api_t* host_api = nullptr;
    void* host_ctx = nullptr;
    
    // Reconnection management
    std::chrono::steady_clock::time_point last_attempt;
    int reconnect_delay_ms = 1000;  // Start with 1 second
    const int max_reconnect_delay_ms = 5000;  // Max 5 seconds
    
    // Statistics
    int frame_count = 0;
    
    // Destructor - clean up resources
    ~RtspContext() {
        cleanup_resources();
        if (hw_device_ctx) {
            av_buffer_unref(&hw_device_ctx);
        }
    }
    
    // Clean up FFmpeg resources
    void cleanup_resources() {
        if (packet) {
            av_packet_free(&packet);
            packet = nullptr;
        }
        
        for (auto& stream : streams) {
            if (stream.codec_ctx) {
                avcodec_free_context(&stream.codec_ctx);
                stream.codec_ctx = nullptr;
            }
        }
        streams.clear();
        
        if (fmt_ctx) {
            avformat_close_input(&fmt_ctx);
            fmt_ctx = nullptr;
        }
    }
    
    // Logging wrapper
    void log(zm_log_level_t level, const char* msg) {
        if (host_api && host_api->log) {
            host_api->log(host_ctx, level, msg);
        }
    }
    
    // Event publishing wrapper
    void publish_event(const char* json) {
        if (host_api && host_api->publish_evt) {
            host_api->publish_evt(host_ctx, json);
        }
    }
    
    // Frame publishing wrapper
    void publish_frame(const zm_frame_hdr_t* hdr, const void* data, size_t size) {
        if (host_api && host_api->on_frame) {
            // Allocate buffer for header + payload
            std::vector<uint8_t> buf(sizeof(zm_frame_hdr_t) + size);
            std::memcpy(buf.data(), hdr, sizeof(zm_frame_hdr_t));
            if (size > 0 && data) {
                std::memcpy(buf.data() + sizeof(zm_frame_hdr_t), data, size);
            }
            char logbuf[256];
            snprintf(logbuf, sizeof(logbuf), "publish_frame: stream_id=%u, size=%zu, pts_usec=%" PRId64 ", flags=0x%x", hdr->stream_id, size, hdr->pts_usec, hdr->flags);
            log(ZM_LOG_DEBUG, logbuf);
            host_api->on_frame(host_ctx, buf.data(), buf.size());
        }
    }
};

// Initialize hardware acceleration context based on available types
static bool init_hw_device(RtspContext* ctx, AVHWDeviceType type, const AVCodec* codec) {
    char buffer[128];
    const char* type_name = av_hwdevice_get_type_name(type);
    
    snprintf(buffer, sizeof(buffer), "Trying hardware acceleration: %s", type_name);
    ctx->log(ZM_LOG_INFO, buffer);
    
    // Check if codec supports this hardware type
    bool supported = false;
    for (int i = 0;; i++) {
        const AVCodecHWConfig* config = avcodec_get_hw_config(codec, i);
        if (!config) {
            break;
        }
        if (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX && 
            config->device_type == type) {
            supported = true;
            break;
        }
    }
    
    if (!supported) {
        snprintf(buffer, sizeof(buffer), "Hardware acceleration not supported for codec with %s", type_name);
        ctx->log(ZM_LOG_INFO, buffer);
        return false;
    }
    
    // Create hardware device context
    int err = av_hwdevice_ctx_create(&ctx->hw_device_ctx, type, nullptr, nullptr, 0);
    if (err < 0) {
        char err_buf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(err, err_buf, sizeof(err_buf));
        snprintf(buffer, sizeof(buffer), "Failed to create hardware device context: %s", err_buf);
        ctx->log(ZM_LOG_WARN, buffer);
        return false;
    }
    
    snprintf(buffer, sizeof(buffer), "Hardware acceleration initialized: %s", type_name);
    ctx->log(ZM_LOG_INFO, buffer);
    return true;
}

// Try to initialize hardware acceleration for the given codec
static bool setup_hw_acceleration(RtspContext* ctx, AVCodecContext* codec_ctx, const AVCodec* codec) {
    // Define preferred hardware acceleration types in order of preference
    AVHWDeviceType hw_types[] = {
#if defined(__APPLE__)
        AV_HWDEVICE_TYPE_VIDEOTOOLBOX,  // Apple platforms
#elif defined(__linux__)
        AV_HWDEVICE_TYPE_VAAPI,         // Linux VA-API
        AV_HWDEVICE_TYPE_CUDA,          // NVIDIA
#elif defined(_WIN32)
        AV_HWDEVICE_TYPE_DXVA2,         // Windows
        AV_HWDEVICE_TYPE_CUDA,          // NVIDIA
#endif
        AV_HWDEVICE_TYPE_NONE
    };
    
    // Try each hardware type until one works
    for (int i = 0; hw_types[i] != AV_HWDEVICE_TYPE_NONE; i++) {
        if (init_hw_device(ctx, hw_types[i], codec)) {
            codec_ctx->hw_device_ctx = av_buffer_ref(ctx->hw_device_ctx);
            return true;
        }
    }
    
    ctx->log(ZM_LOG_INFO, "No hardware acceleration available, using software decoding");
    return false;
}

// Map FFmpeg hardware type to ZoneMinder hardware type
static zm_hw_type_t map_hw_type(AVHWDeviceType type) {
    switch (type) {
        case AV_HWDEVICE_TYPE_CUDA:
            return ZM_HW_CUDA;
        case AV_HWDEVICE_TYPE_VAAPI:
            return ZM_HW_VAAPI;
        case AV_HWDEVICE_TYPE_VIDEOTOOLBOX:
            return ZM_HW_VTB;
        case AV_HWDEVICE_TYPE_DXVA2:
            return ZM_HW_DXVA;
        default:
            return ZM_HW_CPU;
    }
}

// Handle frame/packet from the input stream
static void handle_packet(RtspContext* ctx, AVPacket* packet) {
    // Find the stream this packet belongs to
    for (size_t i = 0; i < ctx->streams.size(); i++) {
        const auto& stream = ctx->streams[i];
        
        if (packet->stream_index == stream.index) {
            // Only forward video packets
            if (stream.type == AVMEDIA_TYPE_VIDEO) {
                // Increment frame counter
                ctx->frame_count++;
                
                // Create frame header
                zm_frame_hdr_t hdr = {0};
                hdr.stream_id = static_cast<uint32_t>(i);  // Our stream index (not FFmpeg's)
                hdr.hw_type = map_hw_type(stream.hw_type);
                
                // Store the host context pointer as void* in handle for non-hardware frames
                // This is a hack for passing the ring buffer reference through the callback
                if (!stream.is_hw_accelerated) {
                    hdr.hw_type = ZM_HW_CPU;
                    hdr.handle = (uint64_t)ctx->host_ctx;
                    hdr.bytes = packet->size;
                } else {
                    // For GPU surfaces, the handle is the opaque surface ID
                    // (this isn't actually used yet in FFmpeg API without decoding, but architecture supports it)
                    hdr.handle = (uint64_t)ctx->host_ctx;
                    hdr.bytes = packet->size;
                }
                
                // Set flags (keyframe, etc.)
                hdr.flags = (packet->flags & AV_PKT_FLAG_KEY) ? 1 : 0;
                
                // Convert pts to microseconds using stream timebase
                AVRational tb = ctx->fmt_ctx->streams[packet->stream_index]->time_base;
                if (packet->pts != AV_NOPTS_VALUE) {
                    hdr.pts_usec = av_rescale_q(packet->pts, tb, {1, 1000000});
                } else if (packet->dts != AV_NOPTS_VALUE) {
                    hdr.pts_usec = av_rescale_q(packet->dts, tb, {1, 1000000});
                } else {
                    hdr.pts_usec = 0;
                }
                
                // Publish the frame to the host
                ctx->publish_frame(&hdr, packet->data, packet->size);
                
                // Log progress occasionally
                if (ctx->frame_count % 300 == 0) {
                    char buffer[64];
                    snprintf(buffer, sizeof(buffer), "Captured %d packets", ctx->frame_count);
                    ctx->log(ZM_LOG_DEBUG, buffer);
                }
            }
            
            // Found matching stream, no need to check others
            break;
        }
    }
}

// Connection attempt with proper error handling
static bool connect_to_stream(RtspContext* ctx) {
    char buffer[256];
    
    // Record attempt time for backoff calculation
    ctx->last_attempt = std::chrono::steady_clock::now();
    
    // Clean up any existing resources first
    ctx->cleanup_resources();
    
    // Allocate packet
    ctx->packet = av_packet_alloc();
    if (!ctx->packet) {
        ctx->log(ZM_LOG_ERROR, "Failed to allocate packet");
        return false;
    }
    
    // Set up format context
    ctx->fmt_ctx = avformat_alloc_context();
    if (!ctx->fmt_ctx) {
        ctx->log(ZM_LOG_ERROR, "Failed to allocate format context");
        return false;
    }
    
    // Set up stream options
    AVDictionary* options = nullptr;
    
    // Set RTSP transport - tcp is more reliable but higher latency
    av_dict_set(&options, "rtsp_transport", ctx->transport.c_str(), 0);
    
    // Set low latency options
    av_dict_set(&options, "max_delay", "500000", 0);        // 500ms max delay
    av_dict_set(&options, "fflags", "nobuffer", 0);         // Don't buffer frames
    av_dict_set(&options, "stimeout", "5000000", 0);        // Socket timeout in microseconds (5s)
    av_dict_set(&options, "reconnect", "1", 0);             // Auto reconnect
    av_dict_set(&options, "reconnect_streamed", "1", 0);    // Auto reconnect for streamed media
    av_dict_set(&options, "reconnect_delay_max", "5", 0);   // Max 5 seconds between reconnection attempts
    
    // Try to open input
    int ret = avformat_open_input(&ctx->fmt_ctx, ctx->url.c_str(), nullptr, &options);
    av_dict_free(&options);
    
    if (ret < 0) {
        char err_buf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, err_buf, sizeof(err_buf));
        snprintf(buffer, sizeof(buffer), "Failed to open input: %s", err_buf);
        ctx->log(ZM_LOG_ERROR, buffer);
        return false;
    }
    
    // Find stream info
    ret = avformat_find_stream_info(ctx->fmt_ctx, nullptr);
    if (ret < 0) {
        char err_buf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, err_buf, sizeof(err_buf));
        snprintf(buffer, sizeof(buffer), "Failed to find stream info: %s", err_buf);
        ctx->log(ZM_LOG_ERROR, buffer);
        return false;
    }
    
    // Process stream information
    int video_count = 0;
    int audio_count = 0;
    ctx->streams.clear();
    
    // Process streams up to max_streams
    for (unsigned i = 0; i < ctx->fmt_ctx->nb_streams; i++) {
        AVStream* stream = ctx->fmt_ctx->streams[i];
        AVMediaType type = stream->codecpar->codec_type;
        
        // Only track video and audio streams
        if (type != AVMEDIA_TYPE_VIDEO && type != AVMEDIA_TYPE_AUDIO) {
            continue;
        }
        
        // Count streams by type
        if (type == AVMEDIA_TYPE_VIDEO) {
            video_count++;
        } else if (type == AVMEDIA_TYPE_AUDIO) {
            audio_count++;
        }
        
        // Skip if we've reached max streams
        if (ctx->streams.size() >= static_cast<size_t>(ctx->max_streams)) {
            continue;
        }
        
        // Find decoder
        const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
        if (!codec) {
            snprintf(buffer, sizeof(buffer), "Unsupported codec for stream %d", i);
            ctx->log(ZM_LOG_WARN, buffer);
            continue;
        }
        
        // Create a stream info record
        RtspContext::StreamInfo info;
        info.index = i;
        info.type = type;
        info.is_hw_accelerated = false;
        info.hw_type = AV_HWDEVICE_TYPE_NONE;
        
        // Allocate codec context
        info.codec_ctx = avcodec_alloc_context3(codec);
        if (!info.codec_ctx) {
            ctx->log(ZM_LOG_WARN, "Failed to allocate codec context");
            continue;
        }
        
        // Copy parameters from stream to codec context
        if (avcodec_parameters_to_context(info.codec_ctx, stream->codecpar) < 0) {
            ctx->log(ZM_LOG_WARN, "Failed to copy codec parameters");
            avcodec_free_context(&info.codec_ctx);
            continue;
        }
        
        // Try to set up hardware acceleration if requested
        if (ctx->hw_decode && type == AVMEDIA_TYPE_VIDEO) {
            info.is_hw_accelerated = setup_hw_acceleration(ctx, info.codec_ctx, codec);
            if (info.is_hw_accelerated && ctx->hw_device_ctx) {
                // In FFmpeg 7.x, av_hwframe_ctx_alloc only takes a device context
                AVBufferRef* hw_frames_ctx = av_hwframe_ctx_alloc(ctx->hw_device_ctx);
                if (hw_frames_ctx) {
                    AVHWFramesContext *frames_ctx = (AVHWFramesContext *)hw_frames_ctx->data;
                    frames_ctx->width = stream->codecpar->width;
                    frames_ctx->height = stream->codecpar->height;
                    frames_ctx->format = static_cast<AVPixelFormat>(stream->codecpar->format);
                    frames_ctx->sw_format = AV_PIX_FMT_YUV420P;
                    
                    if (av_hwframe_ctx_init(hw_frames_ctx) >= 0) {
                        info.codec_ctx->hw_frames_ctx = av_buffer_ref(hw_frames_ctx);
                        info.hw_type = ctx->hw_device_ctx->data ? 
                            reinterpret_cast<AVHWDeviceContext*>(ctx->hw_device_ctx->data)->type : 
                            AV_HWDEVICE_TYPE_NONE;
                    }
                    av_buffer_unref(&hw_frames_ctx);
                }
            }
        }
        
        // Add stream to our list
        ctx->streams.push_back(info);
    }
    
    // Log stream info
    snprintf(buffer, sizeof(buffer), "RTSP connected (%d video, %d audio)", video_count, audio_count);
    ctx->log(ZM_LOG_INFO, buffer);
    
    // Publish connection event
    char json_event[512];
    snprintf(json_event, sizeof(json_event), 
            "{\"event\":\"StreamConnected\",\"url\":\"%s\",\"video_streams\":%d,\"audio_streams\":%d}", 
            ctx->url.c_str(), video_count, audio_count);
    ctx->publish_event(json_event);
    // Publish one-time stream metadata for each video stream
    for (size_t si = 0; si < ctx->streams.size(); ++si) {
        const auto& s = ctx->streams[si];
        if (s.type != AVMEDIA_TYPE_VIDEO) continue;
        AVCodecParameters* cp = ctx->fmt_ctx->streams[s.index]->codecpar;
        // Log extradata size for diagnostics
        char extradata_log[256];
        snprintf(extradata_log, sizeof(extradata_log), "[RTSP] Stream %zu: codec_id=%d, width=%d, height=%d, pix_fmt=%d, profile=%d, level=%d, extradata_size=%d", si, cp->codec_id, cp->width, cp->height, cp->format, cp->profile, cp->level, cp->extradata_size);
        ctx->log(ZM_LOG_INFO, extradata_log);
        // If extradata is missing for H.264, try to extract SPS/PPS from the first keyframe
        if (cp->codec_id == AV_CODEC_ID_H264 && cp->extradata_size == 0) {
            ctx->log(ZM_LOG_WARN, "[RTSP] WARNING: extradata (SPS/PPS) is missing for this stream! Attempting to extract from first keyframe.");
            // Wait for the first keyframe packet and extract SPS/PPS
            bool found = false;
            for (int pkt_idx = 0; pkt_idx < 100 && !found; ++pkt_idx) {
                AVPacket* pkt = av_packet_alloc();
                if (av_read_frame(ctx->fmt_ctx, pkt) >= 0) {
                    if (pkt->stream_index == (int)s.index && (pkt->flags & AV_PKT_FLAG_KEY)) {
                        // Try to extract SPS/PPS from this keyframe
                        std::vector<uint8_t> sps, pps;
                        const uint8_t* data = pkt->data;
                        size_t size = pkt->size;
                        size_t i = 0;
                        while (i + 4 < size) {
                            // Look for start code 0x00000001
                            if (data[i] == 0x00 && data[i+1] == 0x00 && data[i+2] == 0x00 && data[i+3] == 0x01) {
                                size_t nal_start = i + 4;
                                uint8_t nal_type = data[nal_start] & 0x1F;
                                size_t nal_end = nal_start;
                                // Find next start code
                                size_t j = nal_start;
                                while (j + 4 < size) {
                                    if (data[j] == 0x00 && data[j+1] == 0x00 && data[j+2] == 0x00 && data[j+3] == 0x01) break;
                                    ++j;
                                }
                                nal_end = j;
                                if (nal_type == 7) { // SPS
                                    sps.assign(data + nal_start, data + nal_end);
                                } else if (nal_type == 8) { // PPS
                                    pps.assign(data + nal_start, data + nal_end);
                                }
                                i = nal_end;
                            } else {
                                ++i;
                            }
                        }
                        if (!sps.empty() && !pps.empty()) {
                            // Build extradata: [start][SPS][start][PPS]
                            std::vector<uint8_t> extradata;
                            static const uint8_t start_code[4] = {0x00, 0x00, 0x00, 0x01};
                            extradata.insert(extradata.end(), start_code, start_code+4);
                            extradata.insert(extradata.end(), sps.begin(), sps.end());
                            extradata.insert(extradata.end(), start_code, start_code+4);
                            extradata.insert(extradata.end(), pps.begin(), pps.end());
                            cp->extradata = (uint8_t*)av_mallocz(extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE);
                            memcpy(cp->extradata, extradata.data(), extradata.size());
                            cp->extradata_size = extradata.size();
                            ctx->log(ZM_LOG_INFO, "[RTSP] Successfully extracted SPS/PPS from first keyframe and set extradata.");
                            found = true;
                        }
                    }
                }
                av_packet_free(&pkt);
            }
            if (!found) {
                ctx->log(ZM_LOG_ERROR, "[RTSP] Failed to extract SPS/PPS from first 100 packets. Filesystem plugin will not be able to mux H.264.");
            }
        }
        // calculate base64 buffer size: 4 * ceil(n/3) + 1 for nul
        int b64len = 4 * ((cp->extradata_size + 2) / 3) + 1;
        std::vector<char> b64buf(b64len);
        av_base64_encode(b64buf.data(), b64len, cp->extradata, cp->extradata_size);
        char meta[1024];
        snprintf(meta, sizeof(meta),
            "{\"event\":\"StreamMetadata\","  \
            "\"stream_id\":%zu,"
            "\"codec_id\":%d,"
            "\"width\":%d,"
            "\"height\":%d,"
            "\"pix_fmt\":%d,"
            "\"profile\":%d,"
            "\"level\":%d,"
            "\"extradata\":\"%s\"}",
            si,
            cp->codec_id,
            cp->width,
            cp->height,
            cp->format,
            cp->profile,
            cp->level,
            b64buf.data());
        ctx->publish_event(meta);
    }
    
    // Reset frame counter and reconnection delay on successful connection
    ctx->frame_count = 0;
    ctx->reconnect_delay_ms = 1000;  // Reset to initial delay
    
    return true;
}

// Main capture thread function
static void capture_thread(RtspContext* ctx) {
    // Connection/reconnection loop
    while (ctx->running) {
        if (!ctx->fmt_ctx || !ctx->packet) {
            // Need to (re)connect
            if (!connect_to_stream(ctx)) {
                // Connection failed, wait with exponential backoff before retrying
                ctx->log(ZM_LOG_WARN, "Connection failed, will retry");
                
                // Sleep for the current reconnection delay
                std::this_thread::sleep_for(std::chrono::milliseconds(ctx->reconnect_delay_ms));
                
                // Increase delay with some randomization (exponential backoff with jitter)
                std::random_device rd;
                std::mt19937 gen(rd());
                std::uniform_int_distribution<> jitter(-200, 200);
                
                ctx->reconnect_delay_ms = std::min(ctx->reconnect_delay_ms * 2 + jitter(gen), 
                                                  ctx->max_reconnect_delay_ms);
                
                continue;
            }
        }
        
        // Read frames from the stream
        int ret = av_read_frame(ctx->fmt_ctx, ctx->packet);
        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                ctx->log(ZM_LOG_INFO, "End of stream reached");
            } else if (ret != AVERROR(EAGAIN)) {
                char err_buf[AV_ERROR_MAX_STRING_SIZE];
                av_strerror(ret, err_buf, sizeof(err_buf));
                char buffer[256];
                snprintf(buffer, sizeof(buffer), "Error reading frame: %s", err_buf);
                ctx->log(ZM_LOG_WARN, buffer);
                
                // Publish reconnection event
                ctx->publish_event("{\"event\":\"StreamReconnecting\"}");
                
                // Clean up and force reconnection
                ctx->cleanup_resources();
                
                // Sleep briefly before reconnection attempt
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            continue;
        }
        
        // Process the packet if it contains data
        handle_packet(ctx, ctx->packet);
        
        // Unref packet for reuse
        av_packet_unref(ctx->packet);
    }
    
    // Clean up when thread exits
    ctx->cleanup_resources();
}

// Simple JSON parsing using string manipulation
static bool parse_json_config(const char* json_cfg, std::string& url, std::string& transport,
                            int& max_streams, bool& hw_decode, zm_host_api_t* host, void* host_ctx) {
    if (!json_cfg || !json_cfg[0]) {
        if (host && host->log) {
            host->log(host_ctx, ZM_LOG_ERROR, "Empty configuration");
        }
        return false;
    }
    
    // Parser that doesn't depend on external JSON libraries
    // In a real implementation, consider using a proper JSON library
    
    // Look for url field
    const char* url_start = strstr(json_cfg, "\"url\"");
    if (url_start) {
        url_start = strchr(url_start + 5, ':');
        if (url_start) {
            url_start++; // Skip colon
            // Skip whitespace and quote
            while (*url_start && (isspace(*url_start) || *url_start == '\"')) url_start++;
            
            // Find end of URL (quote)
            const char* url_end = strchr(url_start, '\"');
            if (url_end) {
                url = std::string(url_start, url_end - url_start);
            }
        }
    }
    
    // Look for transport field
    const char* transport_start = strstr(json_cfg, "\"transport\"");
    if (transport_start) {
        transport_start = strchr(transport_start + 11, ':');
        if (transport_start) {
            transport_start++; // Skip colon
            // Skip whitespace and quote
            while (*transport_start && (isspace(*transport_start) || *transport_start == '\"')) transport_start++;
            
            // Find end of transport (quote)
            const char* transport_end = strchr(transport_start, '\"');
            if (transport_end) {
                transport = std::string(transport_start, transport_end - transport_start);
            }
        }
    }
    
    // Look for max_streams field (integer)
    const char* max_streams_start = strstr(json_cfg, "\"max_streams\"");
    if (max_streams_start) {
        max_streams_start = strchr(max_streams_start + 13, ':');
        if (max_streams_start) {
            max_streams_start++; // Skip colon
            // Skip whitespace
            while (*max_streams_start && isspace(*max_streams_start)) max_streams_start++;
            
            // Convert to integer
            char* end_ptr = nullptr;
            int value = static_cast<int>(strtol(max_streams_start, &end_ptr, 10));
            if (end_ptr != max_streams_start) {
                max_streams = value;
            }
        }
    }
    
    // Look for hw_decode field (boolean)
    const char* hw_decode_start = strstr(json_cfg, "\"hw_decode\"");
    if (hw_decode_start) {
        hw_decode_start = strchr(hw_decode_start + 11, ':');
        if (hw_decode_start) {
            hw_decode_start++; // Skip colon
            // Skip whitespace
            while (*hw_decode_start && isspace(*hw_decode_start)) hw_decode_start++;
            
            // Check if true or false
            if (strncmp(hw_decode_start, "true", 4) == 0) {
                hw_decode = true;
            } else if (strncmp(hw_decode_start, "false", 5) == 0) {
                hw_decode = false;
            }
        }
    }
    
    // URL is required
    if (url.empty()) {
        if (host && host->log) {
            host->log(host_ctx, ZM_LOG_ERROR, "No URL specified in configuration");
        }
        return false;
    }
    
    return true;
}

// Plugin start function - called by the host to start the plugin
extern "C" int rtsp_start(zm_plugin_t* plugin, zm_host_api_t* host_api, void* host_ctx, const char* json_cfg) {
    if (!plugin || !host_api || !json_cfg) {
        return -1;
    }

    // Log host_api pointer and on_frame value for debugging
    char dbg[256];
    snprintf(dbg, sizeof(dbg), "rtsp_start: host_api=%p, on_frame=%p, log=%p, publish_evt=%p", (void*)host_api, host_api ? (void*)host_api->on_frame : nullptr, host_api ? (void*)host_api->log : nullptr, host_api ? (void*)host_api->publish_evt : nullptr);
    if (host_api && host_api->log) host_api->log(host_ctx, ZM_LOG_INFO, dbg);

    // Create new context
    RtspContext* ctx = new RtspContext();
    ctx->host_api = host_api;
    ctx->host_ctx = host_ctx;

    // Parse configuration
    if (!parse_json_config(json_cfg, ctx->url, ctx->transport, ctx->max_streams, 
                          ctx->hw_decode, host_api, host_ctx)) {
        delete ctx;
        return -1;
    }

    // Log startup
    char buffer[256];
    snprintf(buffer, sizeof(buffer), "Starting RTSP plugin with URL: %s", ctx->url.c_str());
    ctx->log(ZM_LOG_INFO, buffer);

    // Start capture thread
    ctx->running = true;
    ctx->worker = std::thread(capture_thread, ctx);

    // Store context in plugin
    plugin->instance = ctx;

    return 0;  // Success
}

// Plugin stop function - called by the host to stop the plugin
extern "C" void rtsp_stop(zm_plugin_t* plugin) {
    if (!plugin || !plugin->instance) {
        return;
    }
    
    RtspContext* ctx = static_cast<RtspContext*>(plugin->instance);
    
    // Signal thread to stop and wait for it
    ctx->log(ZM_LOG_INFO, "Stopping RTSP plugin");
    ctx->running = false;
    if (ctx->worker.joinable()) {
        ctx->worker.join();
    }
    
    // Clean up
    delete ctx;
    plugin->instance = nullptr;
}

// Plugin initialization function - called by the host to initialize the plugin interface
extern "C" __attribute__((visibility("default"))) void zm_plugin_init(zm_plugin_t* plugin) {
    if (!plugin) {
        return;
    }
    // Fill in plugin interface
    plugin->version = 1;
    plugin->type = ZM_PLUGIN_INPUT;
    plugin->instance = nullptr;

    // Set callbacks
    plugin->start = rtsp_start;
    plugin->stop = rtsp_stop;
    plugin->on_frame = nullptr;  // Input plugins don't need this
}
