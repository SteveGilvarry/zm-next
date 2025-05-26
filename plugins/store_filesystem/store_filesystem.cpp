
#include <zm_plugin.h>
#include <nlohmann/json.hpp>
#include <format>
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
#include <filesystem>
#include <fstream>
#include <chrono>
#include <ctime>
#include <string>
#include <vector>
#include <memory>
#include <mutex>

using json = nlohmann::json;
namespace fs = std::filesystem;

struct StoreInstance {
    std::string root;
    int monitor_id = 0;
    int max_secs = 300;
    int flags = 0;
    int hw_encode = 0;
    std::string cur_dir;
    std::string cur_path;
    std::unique_ptr<AVFormatContext, decltype(&avformat_free_context)> fmt_ctx{nullptr, avformat_free_context};
    int64_t start_ts = 0;
    int64_t last_pts = 0;
    bool file_open = false;
    bool warned_gpu = false;
    std::mutex mtx;
    bool header_written = false;
    bool waiting_for_keyframe = false;
    AVPacket* last_keyframe   = nullptr;
    AVStream* video_stream = nullptr;
    // metadata from input plugin
    AVCodecParameters* metadata_codecpar = nullptr;
    zm_host_api_t* host = nullptr;
    void* host_ctx = nullptr;
    // Multi-stream support
    std::vector<uint32_t> stream_filter;  // If empty, accept all streams
};

static std::string get_default_root() {
#ifdef __APPLE__
    return "/Shared/zm/media";
#elif defined(_WIN32)
    return "C:/ZM/media";
#else
    return "/lib/zm/media";
#endif
}


static std::string make_path(const std::string& root, int monitor_id, std::time_t t) {
    std::tm tm = *std::localtime(&t);
    return std::format("{}/{:04d}-{:02d}-{:02d}/Monitor-{}/{:02d}-{:02d}-{:02d}.mkv",
                       root,
                       tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                       monitor_id,
                       tm.tm_hour, tm.tm_min, tm.tm_sec);
}

static void log(StoreInstance* inst, zm_log_level_t level, const char* fmt, ...) {
    if (!inst || !inst->host || !inst->host->log) return;
    va_list ap;
    va_start(ap, fmt);
    char buf[512];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    inst->host->log(inst->host_ctx, level, buf);
}

static void close_file(StoreInstance* inst) {
    if (!inst->file_open) return;
    // Free the last keyframe if it exists
    if (inst->last_keyframe) {
        av_packet_free(&inst->last_keyframe);
        inst->last_keyframe = nullptr;
    }
    av_write_trailer(inst->fmt_ctx.get());
    avio_closep(&inst->fmt_ctx->pb);
    int64_t duration = inst->last_pts - inst->start_ts;
    json ev = { {"path", inst->cur_path}, {"duration", duration} };
    if (inst->host && inst->host->publish_evt)
        inst->host->publish_evt(inst->host_ctx, ev.dump().c_str());
    log(inst, ZM_LOG_INFO, "Closed file: %s (duration=%ld)", inst->cur_path.c_str(), duration);
    inst->fmt_ctx.reset();
    inst->file_open = false;
    inst->header_written = false;
    inst->video_stream = nullptr;
}

static bool open_file(StoreInstance* inst, std::time_t t) {
    inst->cur_path = make_path(inst->root, inst->monitor_id, t);
    fs::create_directories(fs::path(inst->cur_path).parent_path());
    AVFormatContext* ctx = nullptr;
    if (avformat_alloc_output_context2(&ctx, nullptr, "matroska", inst->cur_path.c_str()) < 0 || !ctx) {
        log(inst, ZM_LOG_ERROR, "Failed to alloc output context for %s", inst->cur_path.c_str());
        return false;
    }
    inst->fmt_ctx.reset(ctx);
    if (avio_open(&ctx->pb, inst->cur_path.c_str(), AVIO_FLAG_WRITE) < 0) {
        log(inst, ZM_LOG_ERROR, "Failed to open file %s", inst->cur_path.c_str());
        return false;
    }
    // Initialize all values properly
    inst->start_ts = 0;  // Will be set on first frame
    inst->last_pts = 0;
    inst->file_open = true;
    inst->warned_gpu = false;
    inst->header_written = false;
    inst->waiting_for_keyframe = false;
    inst->video_stream = nullptr;
    log(inst, ZM_LOG_INFO, "Opened file: %s", inst->cur_path.c_str());
    return true;
}

// Forward declarations of the handler functions
static int handle_plugin_start(zm_plugin_t* plugin, zm_host_api_t* host, void* host_ctx, const char* json_cfg);
static void handle_frame(zm_plugin_t* plugin, const void* buf, size_t size);
static void handle_plugin_stop(zm_plugin_t* plugin);
static void process_metadata_json(StoreInstance* inst, const char* buf, size_t size);
static void process_video_frame(StoreInstance* inst, const zm_frame_hdr_t* hdr, const uint8_t* payload);
static bool initialize_video_stream(StoreInstance* inst);
static void cache_keyframe(StoreInstance* inst, const zm_frame_hdr_t* hdr, const uint8_t* payload);
static void write_frame_to_file(StoreInstance* inst, const zm_frame_hdr_t* hdr, const uint8_t* payload);
static void check_segment_rotation(StoreInstance* inst);

// The main plugin initialization function
extern "C" __attribute__((visibility("default"))) void zm_plugin_init(zm_plugin_t* plug) {
    plug->version = 1;
    plug->type = ZM_PLUGIN_STORE;
    plug->instance = nullptr;
    plug->start = handle_plugin_start;
    plug->on_frame = handle_frame;
    plug->stop = handle_plugin_stop;
}

// Implementation of the start handler
static int handle_plugin_start(zm_plugin_t* plugin, zm_host_api_t* host, void* host_ctx, const char* json_cfg) {
    auto inst = new StoreInstance;
    inst->host = host;
    inst->host_ctx = host_ctx;
    inst->last_keyframe = nullptr;  // Initialize to nullptr
    inst->metadata_codecpar = nullptr;  // Initialize to nullptr
    inst->video_stream = nullptr;
    try {
        auto j = json::parse(json_cfg);
        inst->root = j.value("root", get_default_root());
        inst->max_secs = j.value("max_secs", 300);
        inst->flags = j.value("flags", 0);
        inst->hw_encode = j.value("hw_encode", 0);
        inst->monitor_id = j.value("monitor_id", 0);
        
        // Parse stream filter if provided
        if (j.contains("stream_filter") && j["stream_filter"].is_array()) {
            for (const auto& sid : j["stream_filter"]) {
                if (sid.is_number_unsigned()) {
                    inst->stream_filter.push_back(sid.get<uint32_t>());
                }
            }
            log(inst, ZM_LOG_INFO, "Stream filter configured for %zu streams", inst->stream_filter.size());
        }
    } catch (...) {
        log(inst, ZM_LOG_ERROR, "Invalid config JSON");
        delete inst;
        return -1;
    }
    std::time_t now = std::time(nullptr);
    if (!open_file(inst, now)) {
        delete inst;
        return -1;
    }
    plugin->instance = inst;
    return 0;
}
// Process JSON metadata from a frame
static void process_metadata_json(StoreInstance* inst, const char* buf, size_t size) {
    try {
        std::string js(buf, size);
        auto j = json::parse(js);
        if (j.value("event", "") == "StreamMetadata") {
            // Check if we should accept metadata for this stream
            uint32_t metadata_stream_id = j.value("stream_id", 0);
            
            // Filter metadata based on stream_filter (if configured)
            if (!inst->stream_filter.empty()) {
                bool should_accept = std::find(inst->stream_filter.begin(), 
                                             inst->stream_filter.end(), 
                                             metadata_stream_id) != inst->stream_filter.end();
                if (!should_accept) {
                    log(inst, ZM_LOG_DEBUG, "Ignoring metadata for stream_id=%u (not in filter list)", metadata_stream_id);
                    return;
                }
            }
            
            log(inst, ZM_LOG_INFO, "Processing metadata for stream_id=%u", metadata_stream_id);
            
            if (inst->metadata_codecpar)
                avcodec_parameters_free(&inst->metadata_codecpar);
            inst->metadata_codecpar = avcodec_parameters_alloc();
            inst->metadata_codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
            inst->metadata_codecpar->codec_id   = j["codec_id"];
            inst->metadata_codecpar->width      = j["width"];
            inst->metadata_codecpar->height     = j["height"];
            inst->metadata_codecpar->format     = j["pix_fmt"];
            inst->metadata_codecpar->profile    = j["profile"];
            inst->metadata_codecpar->level      = j["level"];
            auto ed_b64 = j["extradata"].get<std::string>();
            if (!ed_b64.empty()) {
                // Calculate max possible output size from Base64 string
                size_t max_out_size = (ed_b64.length() * 3) / 4 + 1;
                uint8_t* out_buf = (uint8_t*)av_mallocz(max_out_size + AV_INPUT_BUFFER_PADDING_SIZE);
                
                if (out_buf) {
                    int decoded_size = av_base64_decode(out_buf, ed_b64.c_str(), max_out_size);
                    
                    if (decoded_size > 0) {
                        inst->metadata_codecpar->extradata = out_buf;
                        inst->metadata_codecpar->extradata_size = decoded_size;
                        
                        // Debug log for H.264 SPS/PPS detection
                        if (inst->metadata_codecpar->codec_id == AV_CODEC_ID_H264) {
                            if (decoded_size > 4 && out_buf[0] == 0x01) {
                                log(inst, ZM_LOG_DEBUG, "H.264 extradata appears to be in AVCC format (correct)");
                            } else if (decoded_size > 4 && out_buf[0] == 0x00 && out_buf[1] == 0x00 && 
                                      out_buf[2] == 0x00 && out_buf[3] == 0x01) {
                                log(inst, ZM_LOG_WARN, "H.264 extradata appears to be in Annex-B format (may cause issues)");
                            } else {
                                log(inst, ZM_LOG_WARN, "H.264 extradata format unrecognized");
                            }
                        }
                        
                        log(inst, ZM_LOG_DEBUG, "Base64 decode: successfully decoded %d bytes of extradata", decoded_size);
                    } else {
                        log(inst, ZM_LOG_ERROR, "Base64 decode failed: got %d bytes", decoded_size);
                        av_free(out_buf);
                        inst->metadata_codecpar->extradata = nullptr;
                        inst->metadata_codecpar->extradata_size = 0;
                    }
                } else {
                    log(inst, ZM_LOG_ERROR, "Failed to allocate memory for extradata");
                    inst->metadata_codecpar->extradata = nullptr;
                    inst->metadata_codecpar->extradata_size = 0;
                }
            } else {
                log(inst, ZM_LOG_WARN, "Empty base64-encoded extradata in JSON");
                inst->metadata_codecpar->extradata = nullptr;
                inst->metadata_codecpar->extradata_size = 0;
            }
            log(inst, ZM_LOG_INFO, "Received StreamMetadata, codec %d %dx%d",
                inst->metadata_codecpar->codec_id,
                inst->metadata_codecpar->width,
                inst->metadata_codecpar->height);
        }
    } catch (...) {
        log(inst, ZM_LOG_WARN, "Failed to parse JSON event in on_frame");
    }
}

// Cache a keyframe for later use
static void cache_keyframe(StoreInstance* inst, const zm_frame_hdr_t* hdr, const uint8_t* payload) {
    // Free old keyframe if it exists
    if (inst->last_keyframe) {
        av_packet_free(&inst->last_keyframe);
        inst->last_keyframe = nullptr;
    }
    
    // Allocate new packet for keyframe
    inst->last_keyframe = av_packet_alloc();
    if (!inst->last_keyframe) {
        log(inst, ZM_LOG_ERROR, "Failed to allocate keyframe packet");
        return;
    }
    
    // Allocate and copy keyframe data
    uint8_t* data_copy = (uint8_t*)av_malloc(hdr->bytes);
    if (!data_copy) {
        log(inst, ZM_LOG_ERROR, "Failed to allocate memory for keyframe data");
        av_packet_free(&inst->last_keyframe);
        inst->last_keyframe = nullptr;
        return;
    }
    
    memcpy(data_copy, payload, hdr->bytes);
    inst->last_keyframe->data = data_copy;
    inst->last_keyframe->size = hdr->bytes;
    inst->last_keyframe->pts = hdr->pts_usec;
    inst->last_keyframe->dts = hdr->pts_usec;
    inst->last_keyframe->flags = AV_PKT_FLAG_KEY;
    
    log(inst, ZM_LOG_DEBUG, "Cached keyframe of size %d at ts %" PRId64, 
        hdr->bytes, hdr->pts_usec);
}

// Initialize and set up video stream
static bool initialize_video_stream(StoreInstance* inst) {
    if (!inst->metadata_codecpar) {
        return false;
    }
    
    AVFormatContext* oc = inst->fmt_ctx.get();
    
    // Create stream
    AVStream* st = avformat_new_stream(oc, nullptr);
    if (!st) {
        log(inst, ZM_LOG_ERROR, "Could not create stream");
        return false;
    }
    
    // Make a deep copy of the codec parameters
    int ret = avcodec_parameters_copy(st->codecpar, inst->metadata_codecpar);
    if (ret < 0) {
        log(inst, ZM_LOG_ERROR, "Failed to copy codec parameters");
        return false;
    }
    
    // Make a deep copy of extradata if present
    if (inst->metadata_codecpar->extradata && inst->metadata_codecpar->extradata_size > 0) {
        // Free any existing extradata in the stream (shouldn't be any, but just in case)
        if (st->codecpar->extradata)
            av_free(st->codecpar->extradata);
            
        // Allocate and copy extradata
        st->codecpar->extradata = (uint8_t*)av_mallocz(inst->metadata_codecpar->extradata_size + 
                                                     AV_INPUT_BUFFER_PADDING_SIZE);
        if (st->codecpar->extradata) {
            memcpy(st->codecpar->extradata, inst->metadata_codecpar->extradata, 
                  inst->metadata_codecpar->extradata_size);
            st->codecpar->extradata_size = inst->metadata_codecpar->extradata_size;
            
            log(inst, ZM_LOG_DEBUG, "Deep copied extradata: %p -> %p, size=%d", 
                inst->metadata_codecpar->extradata, st->codecpar->extradata,
                st->codecpar->extradata_size);
        } else {
            log(inst, ZM_LOG_ERROR, "Failed to allocate memory for extradata copy");
            st->codecpar->extradata_size = 0;
        }
    }
    
    st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    st->time_base = AVRational{1,1000000};
    st->avg_frame_rate = (AVRational){25, 1};
    st->r_frame_rate   = st->avg_frame_rate;
    oc->streams[st->index]->time_base = st->time_base;
    
    ret = avformat_write_header(oc, nullptr);
    if (ret < 0) {
        log(inst, ZM_LOG_ERROR, "write_header failed: %s", av_err2str(ret));
        return false;
    }
    
    inst->video_stream = st;
    inst->header_written = true;
    
    return true;
}

// Write a frame to the output file
static void write_frame_to_file(StoreInstance* inst,
                                const zm_frame_hdr_t* hdr,
                                const uint8_t* payload)
{
    log(inst, ZM_LOG_DEBUG, "write_frame_to_file: ENTRY - size=%d, pts=%ld, flags=0x%x", 
        hdr->bytes, hdr->pts_usec, hdr->flags);

    if (!inst->file_open || !inst->header_written || !inst->video_stream) {
        log(inst, ZM_LOG_DEBUG, "write_frame_to_file: SKIP - file_open=%d, header_written=%d, video_stream=%p", 
            inst->file_open, inst->header_written, inst->video_stream);
        return; // Can't write if file isn't open or header isn't written
    } 

    AVPacket* pkt = av_packet_alloc();
    if (!pkt) {
        log(inst, ZM_LOG_ERROR, "Failed to allocate packet");
        return;
    }

    // allocate internal buffer
    if (av_new_packet(pkt, hdr->bytes) < 0) {
        log(inst, ZM_LOG_ERROR, "Failed to allocate packet data");
        av_packet_free(&pkt);
        return;
    }
    
    memcpy(pkt->data, payload, hdr->bytes);

    int64_t relative_pts = hdr->pts_usec - inst->start_ts;
    pkt->pts = pkt->dts = av_rescale_q(relative_pts,
                                       (AVRational){1, 1000000}, // source: microseconds  
                                       inst->video_stream->time_base); // target: stream timebase
    pkt->stream_index = inst->video_stream->index;
    
    if (hdr->flags & 1) {
        pkt->flags |= AV_PKT_FLAG_KEY;
        log(inst, ZM_LOG_DEBUG, "Writing keyframe: size=%d, pts=%" PRId64,
            hdr->bytes, pkt->pts);
    } else {
        log(inst, ZM_LOG_DEBUG, "Writing P/B frame: size=%d, pts=%" PRId64,
            hdr->bytes, pkt->pts);
    }
    
    int ret = av_interleaved_write_frame(inst->fmt_ctx.get(), pkt);
    if (ret < 0) {
        char err_buf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, err_buf, sizeof(err_buf));
        log(inst, ZM_LOG_ERROR, "Error writing frame: %s", err_buf);
    } else {
        log(inst, ZM_LOG_DEBUG, "Successfully wrote frame");
    }
    
    inst->last_pts = hdr->pts_usec;
    av_packet_free(&pkt);
}


// Check if segment needs rotation
static void check_segment_rotation(StoreInstance* inst, const zm_frame_hdr_t* hdr) {
    if (!inst->file_open) {
        return; // Can't rotate if file isn't open
    }
    
    inst->last_pts = hdr->pts_usec;   // keep this fresh for rotation
    // compute elapsed based on recorded timestamps
    int64_t elapsed = (inst->last_pts - inst->start_ts) / 1000000;
    std::time_t now = std::time(nullptr);
    std::tm tm = *std::localtime(&now);
    
    if (elapsed >= inst->max_secs || (tm.tm_hour == 0 && tm.tm_min == 0 && tm.tm_sec < 2)) {
        log(inst, ZM_LOG_INFO, "Segment duration reached or midnight, closing and opening new file");
        
        close_file(inst);
        open_file(inst, now);
        
        inst->header_written = false;
        inst->waiting_for_keyframe = false;
    }
}

// Process an actual video frame
static void process_video_frame(StoreInstance* inst, const zm_frame_hdr_t* hdr, const uint8_t* payload) {
    log(inst, ZM_LOG_DEBUG, "process_video_frame: ENTRY stream_id=%d, size=%d, flags=0x%x", 
        hdr->stream_id, hdr->bytes, hdr->flags);
    std::lock_guard<std::mutex> lock(inst->mtx);
    
    if (!inst->file_open) {
        log(inst, ZM_LOG_DEBUG, "on_frame: file not open, skipping");
        return;
    }
    
    // Handle keyframes
    if (hdr->flags & 1) {
        cache_keyframe(inst, hdr, payload);
    }
    
    // Write file header if necessary
    if (!inst->header_written && inst->metadata_codecpar) {
        if (!initialize_video_stream(inst)) {
            return;
        }
        
        // Set initial timestamp if not set
        if (inst->start_ts == 0) {
            inst->start_ts = hdr->pts_usec;
            log(inst, ZM_LOG_INFO, "Setting initial timestamp: %" PRId64, inst->start_ts);
        }
        
        // Write cached keyframe if we have one
        if (inst->last_keyframe) {
            inst->last_keyframe->stream_index = inst->video_stream->index;
            int ret = av_interleaved_write_frame(inst->fmt_ctx.get(), inst->last_keyframe);
            if (ret < 0) {
                log(inst, ZM_LOG_ERROR, "Failed to write cached keyframe: %s", av_err2str(ret));
            } else {
                log(inst, ZM_LOG_INFO, "Successfully wrote cached keyframe");
            }
        } else {
            // Otherwise wait for next keyframe
            inst->waiting_for_keyframe = true;
            log(inst, ZM_LOG_INFO, "No keyframe available, waiting for next keyframe");
            return;
        }
    }
    
    // Write the current frame if all conditions are met:
    // 1. Header is written (which implies we have codec metadata)
    // 2. Video stream is properly initialized
    // 3. We've seen at least one keyframe (header_written also implies this in our implementation)
    if (inst->header_written && inst->video_stream) {
        write_frame_to_file(inst, hdr, payload);
        
        // Only update timestamps and check rotation once we're properly writing frames
        check_segment_rotation(inst, hdr);
    }
}

// Implementation of the frame handler
static void handle_frame(zm_plugin_t* plugin, const void* buf, size_t size) {
    auto inst = static_cast<StoreInstance*>(plugin->instance);
    log(inst, ZM_LOG_DEBUG, "handle_frame:on_frame: inst=%p, buf=%p, size=%zu", inst, buf, size);
    
    if (!inst || !buf) return;
    
    // Detect JSON metadata event (buffer starts with '{')
    if (size > 0 && static_cast<const char*>(buf)[0] == '{') {
        process_metadata_json(inst, static_cast<const char*>(buf), size);
        return;
    }
    
    // Must have a full frame header
    if (size < sizeof(zm_frame_hdr_t)) {
        log(inst, ZM_LOG_ERROR, "handle_frame:on_frame: invalid data size %zu", size);
        return;
    }
    
    const zm_frame_hdr_t* hdr = static_cast<const zm_frame_hdr_t*>(buf);
    
    // Validate frame header values
    if (hdr->bytes == 0) {
        log(inst, ZM_LOG_ERROR, "Invalid frame: zero bytes in payload");
        return;
    }
    
    // Skip frames until we get a keyframe to start segment
    if (inst->waiting_for_keyframe && !(hdr->flags & 1)) {
        log(inst, ZM_LOG_DEBUG, "Waiting for keyframe, skipping non-keyframe");
        return;
    }
    
    // We have our starting keyframe or header already written
    if (inst->waiting_for_keyframe) {
        inst->waiting_for_keyframe = false;
        log(inst, ZM_LOG_INFO, "Got keyframe, resuming processing");
    }
    
    log(inst, ZM_LOG_DEBUG, "handle_frame:on_frame: stream_id=%u, bytes=%u, pts_usec=%" PRId64 ", flags=0x%x, hw_type=%d", 
        hdr->stream_id, hdr->bytes, hdr->pts_usec, hdr->flags, hdr->hw_type);
    
    // Check stream filter - if configured, only accept specified streams
    if (!inst->stream_filter.empty()) {
        bool stream_allowed = false;
        for (uint32_t allowed_stream : inst->stream_filter) {
            if (hdr->stream_id == allowed_stream) {
                stream_allowed = true;
                break;
            }
        }
        if (!stream_allowed) {
            log(inst, ZM_LOG_DEBUG, "Filtering out stream_id=%u (not in allowed list)", hdr->stream_id);
            return;
        }
    }
    
    // Skip GPU frames
    if (hdr->hw_type != ZM_HW_CPU) {
        if (!inst->warned_gpu) {
            log(inst, ZM_LOG_WARN, "Skipping GPU frame");
            inst->warned_gpu = true;
        }
        return;
    }
    
    // Check payload size
    if (size < sizeof(zm_frame_hdr_t) + hdr->bytes) {
        log(inst, ZM_LOG_ERROR, "Frame buffer too small: got %zu, need %zu", 
            size, sizeof(zm_frame_hdr_t) + static_cast<size_t>(hdr->bytes));
        return;
    }
    
    const uint8_t* payload = static_cast<const uint8_t*>(buf) + sizeof(zm_frame_hdr_t);
    
    // Process the video frame
    process_video_frame(inst, hdr, payload);
}

// Implementation of the plugin stop handler
static void handle_plugin_stop(zm_plugin_t* plugin) {
    auto inst = static_cast<StoreInstance*>(plugin->instance);
    if (inst) {
        if (inst->metadata_codecpar) {
            avcodec_parameters_free(&inst->metadata_codecpar);
        }
        close_file(inst);
        delete inst;
        plugin->instance = nullptr;
    }
}
