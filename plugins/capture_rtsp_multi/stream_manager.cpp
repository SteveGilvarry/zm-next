#include "stream_manager.hpp"
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cinttypes>  // for PRId64
#include <algorithm>
#include <sstream>

extern "C" {
#include <libavutil/base64.h>  // for av_base64_encode
}

StreamManager::StreamManager() 
    : host_api_(nullptr), host_ctx_(nullptr), global_hw_decode_(false), 
      default_transport_("tcp"), preferred_hw_type_(AV_HWDEVICE_TYPE_NONE),
      gen_(rd_()), jitter_dist_(-200, 200) {
}

StreamManager::~StreamManager() {
    stop_all_streams();
}

bool StreamManager::initialize(zm_host_api_t* host_api, void* host_ctx, const char* json_config) {
    host_api_ = host_api;
    host_ctx_ = host_ctx;
    
    log(ZM_LOG_INFO, "Initializing multi-stream RTSP capture manager");
    
    // Parse configuration
    if (!parse_configuration(json_config)) {
        log(ZM_LOG_ERROR, "Failed to parse configuration");
        return false;
    }
    
    // Determine preferred hardware acceleration
#if defined(__APPLE__)
    preferred_hw_type_ = AV_HWDEVICE_TYPE_VIDEOTOOLBOX;
#elif defined(__linux__)
    preferred_hw_type_ = AV_HWDEVICE_TYPE_VAAPI;
#else
    preferred_hw_type_ = AV_HWDEVICE_TYPE_NONE;
#endif
    
    log(ZM_LOG_INFO, "Multi-stream RTSP manager initialized with %zu streams", stream_configs_.size());
    return true;
}

bool StreamManager::parse_configuration(const char* json_config) {
    // Simplified JSON parsing for demonstration
    // In production, use a proper JSON library like nlohmann/json
    
    if (!json_config) {
        log(ZM_LOG_ERROR, "No configuration provided");
        return false;
    }
    
    log(ZM_LOG_DEBUG, "Parsing configuration: %s", json_config);
    
    // For now, implement basic parsing for common cases
    // This is a simplified parser - replace with proper JSON library
    std::string config_str(json_config);
    
    // Look for "streams" array
    size_t streams_pos = config_str.find("\"streams\"");
    if (streams_pos == std::string::npos) {
        // Single stream configuration for backward compatibility
        StreamConfig config;
        config.stream_id = 0;
        
        // Extract URL
        size_t url_start = config_str.find("\"url\"");
        if (url_start != std::string::npos) {
            url_start = config_str.find(":", url_start) + 1;
            url_start = config_str.find("\"", url_start) + 1;
            size_t url_end = config_str.find("\"", url_start);
            config.url = config_str.substr(url_start, url_end - url_start);
        }
        
        // Extract transport
        size_t transport_start = config_str.find("\"transport\"");
        if (transport_start != std::string::npos) {
            transport_start = config_str.find(":", transport_start) + 1;
            transport_start = config_str.find("\"", transport_start) + 1;
            size_t transport_end = config_str.find("\"", transport_start);
            config.transport = config_str.substr(transport_start, transport_end - transport_start);
        } else {
            config.transport = default_transport_;
        }
        
        // Extract hw_decode
        size_t hw_pos = config_str.find("\"hw_decode\"");
        if (hw_pos != std::string::npos) {
            size_t value_pos = config_str.find(":", hw_pos) + 1;
            config.hw_decode = (config_str.find("true", value_pos) != std::string::npos);
        } else {
            config.hw_decode = global_hw_decode_;
        }
        
        if (!config.url.empty()) {
            stream_configs_[config.stream_id] = config;
            log(ZM_LOG_INFO, "Added single stream: %s (transport: %s, hw_decode: %s)", 
                config.url.c_str(), config.transport.c_str(), config.hw_decode ? "true" : "false");
        }
    } else {
        // Multi-stream configuration
        log(ZM_LOG_INFO, "Multi-stream configuration detected");
        
        // This is a simplified parser - in production use proper JSON library
        // For now, assume format like:
        // {"streams": [{"url": "rtsp://...", "stream_id": 0}, {"url": "rtsp://...", "stream_id": 1}]}
        
        // Extract each stream entry
        size_t stream_start = 0;
        uint32_t stream_counter = 0;
        
        while ((stream_start = config_str.find("{", stream_start)) != std::string::npos) {
            size_t stream_end = config_str.find("}", stream_start);
            if (stream_end == std::string::npos) break;
            
            std::string stream_json = config_str.substr(stream_start, stream_end - stream_start + 1);
            
            // Check if this contains a URL (indicates it's a stream object)
            if (stream_json.find("\"url\"") != std::string::npos) {
                StreamConfig config;
                config.stream_id = stream_counter++;
                
                // Extract URL
                size_t url_start = stream_json.find("\"url\"");
                if (url_start != std::string::npos) {
                    url_start = stream_json.find(":", url_start) + 1;
                    url_start = stream_json.find("\"", url_start) + 1;
                    size_t url_end = stream_json.find("\"", url_start);
                    config.url = stream_json.substr(url_start, url_end - url_start);
                }
                
                // Extract stream_id if specified
                size_t id_pos = stream_json.find("\"stream_id\"");
                if (id_pos != std::string::npos) {
                    size_t id_start = stream_json.find(":", id_pos) + 1;
                    size_t id_end = stream_json.find_first_of(",}", id_start);
                    std::string id_str = stream_json.substr(id_start, id_end - id_start);
                    // Remove whitespace
                    id_str.erase(std::remove_if(id_str.begin(), id_str.end(), ::isspace), id_str.end());
                    config.stream_id = std::stoul(id_str);
                }
                
                // Extract transport
                size_t transport_start = stream_json.find("\"transport\"");
                if (transport_start != std::string::npos) {
                    transport_start = stream_json.find(":", transport_start) + 1;
                    transport_start = stream_json.find("\"", transport_start) + 1;
                    size_t transport_end = stream_json.find("\"", transport_start);
                    config.transport = stream_json.substr(transport_start, transport_end - transport_start);
                } else {
                    config.transport = default_transport_;
                }
                
                // Extract hw_decode
                size_t hw_pos = stream_json.find("\"hw_decode\"");
                if (hw_pos != std::string::npos) {
                    size_t value_pos = stream_json.find(":", hw_pos) + 1;
                    config.hw_decode = (stream_json.find("true", value_pos) != std::string::npos);
                } else {
                    config.hw_decode = global_hw_decode_;
                }
                
                if (!config.url.empty()) {
                    stream_configs_[config.stream_id] = config;
                    log(ZM_LOG_INFO, "Added stream %u: %s (transport: %s, hw_decode: %s)", 
                        config.stream_id, config.url.c_str(), config.transport.c_str(), 
                        config.hw_decode ? "true" : "false");
                }
            }
            
            stream_start = stream_end + 1;
        }
    }
    
    return !stream_configs_.empty();
}

bool StreamManager::start_all_streams() {
    std::lock_guard<std::mutex> lock(streams_mutex_);
    
    log(ZM_LOG_INFO, "Starting %zu RTSP streams", stream_configs_.size());
    
    bool all_started = true;
    for (const auto& [stream_id, config] : stream_configs_) {
        if (!setup_stream(stream_id)) {
            log(ZM_LOG_ERROR, "Failed to setup stream %u", stream_id);
            all_started = false;
            continue;
        }
        
        auto& state = stream_states_[stream_id];
        state->running = true;
        state->capture_thread = std::thread(&StreamManager::capture_loop, this, stream_id);
        
        log(ZM_LOG_INFO, "Started capture thread for stream %u", stream_id);
    }
    
    return all_started;
}

void StreamManager::stop_all_streams() {
    std::lock_guard<std::mutex> lock(streams_mutex_);
    
    log(ZM_LOG_INFO, "Stopping all RTSP streams");
    
    // Signal all threads to stop
    for (auto& [stream_id, state] : stream_states_) {
        state->running = false;
    }
    
    // Wait for all threads to finish
    for (auto& [stream_id, state] : stream_states_) {
        if (state->capture_thread.joinable()) {
            state->capture_thread.join();
        }
        cleanup_stream(stream_id);
    }
    
    stream_states_.clear();
    log(ZM_LOG_INFO, "All streams stopped");
}

bool StreamManager::setup_stream(uint32_t stream_id) {
    auto config_it = stream_configs_.find(stream_id);
    if (config_it == stream_configs_.end()) {
        log(ZM_LOG_ERROR, "No configuration found for stream %u", stream_id);
        return false;
    }
    
    auto state = std::make_unique<StreamState>();
    state->stream_id = stream_id;
    state->start_time = std::chrono::steady_clock::now();
    
    stream_states_[stream_id] = std::move(state);
    
    log_stream(stream_id, ZM_LOG_INFO, "Stream setup completed");
    return true;
}

void StreamManager::cleanup_stream(uint32_t stream_id) {
    auto state_it = stream_states_.find(stream_id);
    if (state_it == stream_states_.end()) return;
    
    auto& state = state_it->second;
    
    if (state->packet) {
        av_packet_free(&state->packet);
    }
    
    if (state->codec_ctx) {
        avcodec_free_context(&state->codec_ctx);
    }
    
    if (state->fmt_ctx) {
        avformat_close_input(&state->fmt_ctx);
    }
    
    if (state->hw_device_ctx) {
        av_buffer_unref((AVBufferRef**)&state->hw_device_ctx);
    }
    
    log_stream(stream_id, ZM_LOG_INFO, "Stream cleanup completed");
}

void StreamManager::capture_loop(uint32_t stream_id) {
    auto config_it = stream_configs_.find(stream_id);
    auto state_it = stream_states_.find(stream_id);
    
    if (config_it == stream_configs_.end() || state_it == stream_states_.end()) {
        log(ZM_LOG_ERROR, "Invalid stream configuration for stream %u", stream_id);
        return;
    }
    
    const auto& config = config_it->second;
    auto& state = state_it->second;
    
    log_stream(stream_id, ZM_LOG_INFO, "Starting capture loop for %s", config.url.c_str());
    
    while (state->running) {
        // Try to connect if not connected
        if (!state->connected) {
            // Record attempt time for backoff calculation
            state->last_attempt_time = std::chrono::steady_clock::now();
            
            if (connect_stream(state.get(), config)) {
                state->connected = true;
                state->retry_count = 0;
                state->current_retry_delay_ms = MIN_RECONNECT_DELAY_MS; // Reset delay on success
                log_stream(stream_id, ZM_LOG_INFO, "Connected successfully");
            } else {
                state->retry_count++;
                if (config.max_retry_attempts > 0 && state->retry_count >= config.max_retry_attempts) {
                    log_stream(stream_id, ZM_LOG_ERROR, "Max retry attempts reached, stopping stream");
                    break;
                }
                
                // Calculate exponential backoff with jitter
                int base_delay = std::min(state->current_retry_delay_ms, MAX_RECONNECT_DELAY_MS);
                int jitter = jitter_dist_(gen_);
                int delay_ms = base_delay + jitter;
                delay_ms = std::max(delay_ms, MIN_RECONNECT_DELAY_MS);
                
                log_stream(stream_id, ZM_LOG_WARN, "Connection failed, retrying in %d ms (attempt %d)", 
                          delay_ms, state->retry_count);
                
                // Increase delay for next attempt (exponential backoff)
                state->current_retry_delay_ms = std::min(state->current_retry_delay_ms * 2, MAX_RECONNECT_DELAY_MS);
                
                std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
                continue;
            }
        }
        
        // Read and process frames
        if (state->connected && state->fmt_ctx) {
            int ret = av_read_frame(state->fmt_ctx, state->packet);
            
            if (ret >= 0) {
                if (state->packet->stream_index == state->video_stream_index) {
                    process_and_publish_frame(state.get(), config);
                    state->frames_captured++;
                } else {
                    // Non-video packet (audio, etc.) - just discard
                    av_packet_unref(state->packet);
                }
                
                // Small delay to prevent overwhelming the system and network
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            } else {
                if (ret == AVERROR_EOF) {
                    log_stream(stream_id, ZM_LOG_INFO, "End of stream reached");
                    handle_stream_disconnect(stream_id);
                } else if (ret == AVERROR(EAGAIN)) {
                    // Temporary unavailability - just wait a bit and continue
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                } else {
                    char err_buf[AV_ERROR_MAX_STRING_SIZE];
                    av_strerror(ret, err_buf, sizeof(err_buf));
                    log_stream(stream_id, ZM_LOG_WARN, "Error reading frame: %s", err_buf);
                    
                    // Publish reconnection event
                    char json_event[256];
                    snprintf(json_event, sizeof(json_event), 
                            "{\"event\":\"StreamReconnecting\",\"stream_id\":%u}", stream_id);
                    if (host_api_ && host_api_->publish_evt) {
                        host_api_->publish_evt(host_ctx_, json_event);
                    }
                    
                    handle_stream_disconnect(stream_id);
                }
            }
        } else {
            // Not connected - wait before next connection attempt
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        
        // Small delay to prevent busy waiting
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    
    log_stream(stream_id, ZM_LOG_INFO, "Capture loop ended");
}

bool StreamManager::connect_stream(StreamState* state, const StreamConfig& config) {
    // Cleanup any existing connection
    if (state->fmt_ctx) {
        avformat_close_input(&state->fmt_ctx);
        state->fmt_ctx = nullptr;
    }
    
    if (state->codec_ctx) {
        avcodec_free_context(&state->codec_ctx);
        state->codec_ctx = nullptr;
    }
    
    // Allocate format context
    state->fmt_ctx = avformat_alloc_context();
    if (!state->fmt_ctx) {
        log_stream(state->stream_id, ZM_LOG_ERROR, "Failed to allocate format context");
        return false;
    }
    
    // Set RTSP options for low latency and reliability
    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "rtsp_transport", config.transport.c_str(), 0);
    av_dict_set(&opts, "rtsp_flags", "prefer_tcp", 0);
    av_dict_set(&opts, "max_delay", "500000", 0);        // 500ms max delay
    av_dict_set(&opts, "fflags", "nobuffer", 0);         // Don't buffer frames
    av_dict_set(&opts, "stimeout", "5000000", 0);        // Socket timeout in microseconds (5s)
    av_dict_set(&opts, "reconnect", "1", 0);             // Auto reconnect
    av_dict_set(&opts, "reconnect_streamed", "1", 0);    // Auto reconnect for streamed media
    av_dict_set(&opts, "reconnect_delay_max", "5", 0);   // Max 5 seconds between reconnection attempts
    
    // Open input
    int ret = avformat_open_input(&state->fmt_ctx, config.url.c_str(), nullptr, &opts);
    av_dict_free(&opts);
    
    if (ret < 0) {
        char err_buf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, err_buf, sizeof(err_buf));
        log_stream(state->stream_id, ZM_LOG_ERROR, "Failed to open RTSP stream: %s", err_buf);
        return false;
    }
    
    // Find stream info
    ret = avformat_find_stream_info(state->fmt_ctx, nullptr);
    if (ret < 0) {
        char err_buf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, err_buf, sizeof(err_buf));
        log_stream(state->stream_id, ZM_LOG_ERROR, "Failed to find stream info: %s", err_buf);
        return false;
    }
    
    // Count streams and find video stream
    int video_count = 0, audio_count = 0;
    state->video_stream_index = -1;
    for (unsigned int i = 0; i < state->fmt_ctx->nb_streams; i++) {
        AVMediaType type = state->fmt_ctx->streams[i]->codecpar->codec_type;
        if (type == AVMEDIA_TYPE_VIDEO) {
            video_count++;
            if (state->video_stream_index == -1) {
                state->video_stream_index = i;
            }
        } else if (type == AVMEDIA_TYPE_AUDIO) {
            audio_count++;
        }
    }
    
    if (state->video_stream_index == -1) {
        log_stream(state->stream_id, ZM_LOG_ERROR, "No video stream found");
        return false;
    }
    
    log_stream(state->stream_id, ZM_LOG_INFO, "Found %d video, %d audio streams", video_count, audio_count);
    
    // Setup decoder
    AVStream* video_stream = state->fmt_ctx->streams[state->video_stream_index];
    const AVCodec* codec = avcodec_find_decoder(video_stream->codecpar->codec_id);
    if (!codec) {
        log_stream(state->stream_id, ZM_LOG_ERROR, "Decoder not found");
        return false;
    }
    
    state->codec_ctx = avcodec_alloc_context3(codec);
    if (!state->codec_ctx) {
        log_stream(state->stream_id, ZM_LOG_ERROR, "Failed to allocate codec context");
        return false;
    }
    
    ret = avcodec_parameters_to_context(state->codec_ctx, video_stream->codecpar);
    if (ret < 0) {
        log_stream(state->stream_id, ZM_LOG_ERROR, "Failed to copy codec parameters");
        return false;
    }
    
    // Setup hardware acceleration if requested
    if (config.hw_decode && preferred_hw_type_ != AV_HWDEVICE_TYPE_NONE) {
        init_hardware_acceleration(state, codec);
    }
    
    // Open codec
    ret = avcodec_open2(state->codec_ctx, codec, nullptr);
    if (ret < 0) {
        char err_buf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, err_buf, sizeof(err_buf));
        log_stream(state->stream_id, ZM_LOG_ERROR, "Failed to open codec: %s", err_buf);
        return false;
    }
    
    // Allocate packet
    state->packet = av_packet_alloc();
    if (!state->packet) {
        log_stream(state->stream_id, ZM_LOG_ERROR, "Failed to allocate packet");
        return false;
    }
    
    log_stream(state->stream_id, ZM_LOG_INFO, "Successfully connected to %s", config.url.c_str());
    
    // Publish connection event with stream info
    char json_event[512];
    snprintf(json_event, sizeof(json_event), 
            "{\"event\":\"StreamConnected\",\"stream_id\":%u,\"url\":\"%s\",\"video_streams\":%d,\"audio_streams\":%d}", 
            config.stream_id, config.url.c_str(), video_count, audio_count);
    if (host_api_ && host_api_->publish_evt) {
        host_api_->publish_evt(host_ctx_, json_event);
    }
    
    // Publish stream metadata with codec parameters
    publish_stream_metadata(config.stream_id, video_stream->codecpar);
    
    return true;
}

bool StreamManager::init_hardware_acceleration(StreamState* state, const AVCodec* codec) {
    // Check if codec supports hardware acceleration
    bool supported = false;
    for (int i = 0;; i++) {
        const AVCodecHWConfig* config = avcodec_get_hw_config(codec, i);
        if (!config) break;
        
        if (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX && 
            config->device_type == preferred_hw_type_) {
            supported = true;
            break;
        }
    }
    
    if (!supported) {
        log_stream(state->stream_id, ZM_LOG_INFO, "Hardware acceleration not supported for this codec");
        return false;
    }
    
    // Create hardware device context
    AVBufferRef* hw_device_ref = nullptr;
    int ret = av_hwdevice_ctx_create(&hw_device_ref, preferred_hw_type_, nullptr, nullptr, 0);
    if (ret < 0) {
        char err_buf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, err_buf, sizeof(err_buf));
        log_stream(state->stream_id, ZM_LOG_WARN, "Failed to create hardware device: %s", err_buf);
        return false;
    }
    
    state->codec_ctx->hw_device_ctx = av_buffer_ref(hw_device_ref);
    state->hw_device_ctx = (AVHWDeviceContext*)hw_device_ref->data;
    
    const char* type_name = av_hwdevice_get_type_name(preferred_hw_type_);
    log_stream(state->stream_id, ZM_LOG_INFO, "Hardware acceleration enabled: %s", type_name);
    
    av_buffer_unref(&hw_device_ref);
    return true;
}

void StreamManager::handle_stream_disconnect(uint32_t stream_id) {
    auto state_it = stream_states_.find(stream_id);
    if (state_it == stream_states_.end()) return;
    
    auto& state = state_it->second;
    state->connected = false;
    
    // Cleanup FFmpeg contexts
    if (state->codec_ctx) {
        avcodec_free_context(&state->codec_ctx);
        state->codec_ctx = nullptr;
    }
    
    if (state->fmt_ctx) {
        avformat_close_input(&state->fmt_ctx);
        state->fmt_ctx = nullptr;
    }
    
    if (state->packet) {
        av_packet_free(&state->packet);
        state->packet = nullptr;
    }
    
    // Publish disconnection event
    char json_event[512];
    snprintf(json_event, sizeof(json_event), 
            "{\"event\":\"StreamDisconnected\",\"stream_id\":%u}", stream_id);
    if (host_api_ && host_api_->publish_evt) {
        host_api_->publish_evt(host_ctx_, json_event);
    }
    
    log_stream(stream_id, ZM_LOG_INFO, "Stream disconnected, will attempt reconnection");
}

void StreamManager::process_and_publish_frame(StreamState* state, const StreamConfig& config) {
    if (!state->packet || !host_api_ || !host_api_->on_frame) {
        return;
    }
    
    // Validate packet data before processing
    if (!state->packet->data || state->packet->size <= 0) {
        log_stream(config.stream_id, ZM_LOG_WARN, "Skipping invalid packet: null data or zero size");
        return;
    }
    
    // Additional validation for reasonable packet size (prevent corruption)
    if (state->packet->size > 10 * 1024 * 1024) {  // 10MB max frame size
        log_stream(config.stream_id, ZM_LOG_WARN, "Skipping oversized packet: %d bytes", state->packet->size);
        return;
    }
    
    // Create frame header
    zm_frame_hdr_t hdr = {};
    hdr.stream_id = config.stream_id;
    hdr.hw_type = config.hw_decode ? (uint32_t)preferred_hw_type_ : ZM_HW_CPU;
    hdr.handle = reinterpret_cast<uint64_t>(state->packet->data);
    hdr.bytes = state->packet->size;
    hdr.flags = (state->packet->flags & AV_PKT_FLAG_KEY) ? 1 : 0;
    
    // Convert PTS to microseconds with validation
    AVStream* stream = state->fmt_ctx->streams[state->video_stream_index];
    if (state->packet->pts != AV_NOPTS_VALUE && stream->time_base.den > 0) {
        hdr.pts_usec = av_rescale_q(state->packet->pts, stream->time_base, {1, 1000000});
    } else if (state->packet->dts != AV_NOPTS_VALUE && stream->time_base.den > 0) {
        // Fallback to DTS if PTS not available
        hdr.pts_usec = av_rescale_q(state->packet->dts, stream->time_base, {1, 1000000});
    } else {
        hdr.pts_usec = av_gettime();  // Use current time as last resort
    }
    
    // Validate the frame data by checking for common codec start patterns
    bool valid_frame = false;
    if (state->packet->size >= 4) {
        const uint8_t* data = state->packet->data;
        
        // Check for H.264 NAL unit start codes (0x00000001 or 0x000001)
        if ((data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x00 && data[3] == 0x01) ||
            (data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x01)) {
            valid_frame = true;
        }
        // Check for MPEG-4 start codes
        else if (data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x01) {
            valid_frame = true;
        }
        // For other codecs or if we can't detect pattern, assume valid
        // (this is conservative - better to pass questionable frames than drop good ones)
        else {
            valid_frame = true;
        }
    } else {
        // Very small packets are suspicious
        log_stream(config.stream_id, ZM_LOG_WARN, "Suspiciously small packet: %d bytes", state->packet->size);
        valid_frame = false;
    }
    
    if (!valid_frame) {
        log_stream(config.stream_id, ZM_LOG_WARN, "Dropping potentially corrupted frame");
        return;
    }
    
    // Allocate frame buffer and copy data
    std::vector<uint8_t> frame_buf(sizeof(zm_frame_hdr_t) + state->packet->size);
    std::memcpy(frame_buf.data(), &hdr, sizeof(zm_frame_hdr_t));
    std::memcpy(frame_buf.data() + sizeof(zm_frame_hdr_t), state->packet->data, state->packet->size);
    
    // Debug logging for keyframes and periodic updates
    if (hdr.flags & 1) {
        log_stream(config.stream_id, ZM_LOG_DEBUG, "Publishing keyframe: size=%u, pts=%" PRId64, 
                   hdr.bytes, hdr.pts_usec);
    } else if (state->frames_captured % 300 == 0) {
        log_stream(config.stream_id, ZM_LOG_DEBUG, "Progress: captured %d frames", 
                   static_cast<int>(state->frames_captured));
    }
    
    // Publish validated frame to pipeline
    host_api_->on_frame(host_ctx_, frame_buf.data(), frame_buf.size());
}

void StreamManager::publish_stream_metadata(uint32_t stream_id, const AVCodecParameters* codecpar) {
    if (!host_api_ || !host_api_->publish_evt || !codecpar) {
        return;
    }
    
    // Base64-encode extradata if present
    std::string extradata_b64;
    if (codecpar->extradata && codecpar->extradata_size > 0) {
        int b64len = 4 * ((codecpar->extradata_size + 2) / 3) + 1;
        std::vector<char> b64buf(b64len);
        av_base64_encode(b64buf.data(), b64len, codecpar->extradata, codecpar->extradata_size);
        extradata_b64 = std::string(b64buf.data());
    }
    
    // Create StreamMetadata JSON event
    char metadata_json[1024];
    snprintf(metadata_json, sizeof(metadata_json),
             "{\"event\":\"StreamMetadata\",\"stream_id\":%u,"
             "\"codec_id\":%d,\"width\":%d,\"height\":%d,"
             "\"pix_fmt\":%d,\"profile\":%d,\"level\":%d,"
             "\"extradata\":\"%s\"}",
             stream_id,
             codecpar->codec_id,
             codecpar->width,
             codecpar->height,
             codecpar->format,
             codecpar->profile,
             codecpar->level,
             extradata_b64.c_str());
    
    host_api_->publish_evt(host_ctx_, metadata_json);
    
    log(ZM_LOG_INFO, "Published metadata for stream %u: %dx%d, codec=%s",
        stream_id, codecpar->width, codecpar->height,
        avcodec_get_name(static_cast<AVCodecID>(codecpar->codec_id)));
}

// Stream management methods
bool StreamManager::add_stream(const StreamConfig& config) {
    std::lock_guard<std::mutex> lock(streams_mutex_);
    
    if (stream_configs_.find(config.stream_id) != stream_configs_.end()) {
        log(ZM_LOG_ERROR, "Stream ID %u already exists", config.stream_id);
        return false;
    }
    
    stream_configs_[config.stream_id] = config;
    log(ZM_LOG_INFO, "Added new stream %u: %s", config.stream_id, config.url.c_str());
    
    return true;
}

bool StreamManager::remove_stream(uint32_t stream_id) {
    std::lock_guard<std::mutex> lock(streams_mutex_);
    
    // Stop the stream if running
    auto state_it = stream_states_.find(stream_id);
    if (state_it != stream_states_.end()) {
        auto& state = state_it->second;
        state->running = false;
        
        if (state->capture_thread.joinable()) {
            state->capture_thread.join();
        }
        
        cleanup_stream(stream_id);
        stream_states_.erase(state_it);
    }
    
    // Remove configuration
    auto config_it = stream_configs_.find(stream_id);
    if (config_it != stream_configs_.end()) {
        stream_configs_.erase(config_it);
        log(ZM_LOG_INFO, "Removed stream %u", stream_id);
        return true;
    }
    
    return false;
}

// Status methods
size_t StreamManager::get_stream_count() const {
    std::lock_guard<std::mutex> lock(streams_mutex_);
    return stream_configs_.size();
}

bool StreamManager::is_stream_connected(uint32_t stream_id) const {
    std::lock_guard<std::mutex> lock(streams_mutex_);
    auto state_it = stream_states_.find(stream_id);
    return state_it != stream_states_.end() && state_it->second->connected;
}

std::vector<uint32_t> StreamManager::get_active_stream_ids() const {
    std::lock_guard<std::mutex> lock(streams_mutex_);
    std::vector<uint32_t> ids;
    for (const auto& [stream_id, config] : stream_configs_) {
        ids.push_back(stream_id);
    }
    return ids;
}

std::vector<StreamManager::StreamStats> StreamManager::get_stream_statistics() const {
    std::lock_guard<std::mutex> lock(streams_mutex_);
    std::vector<StreamStats> stats;
    
    for (const auto& [stream_id, config] : stream_configs_) {
        StreamStats stat = {};
        stat.stream_id = stream_id;
        
        auto state_it = stream_states_.find(stream_id);
        if (state_it != stream_states_.end()) {
            const auto& state = state_it->second;
            stat.connected = state->connected;
            stat.frames_captured = state->frames_captured;
            stat.packets_dropped = state->packets_dropped;
            stat.retry_count = state->retry_count;
            
            auto now = std::chrono::steady_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - state->start_time);
            stat.uptime_seconds = duration.count() / 1000.0;
        }
        
        stats.push_back(stat);
    }
    
    return stats;
}

// Logging methods
void StreamManager::log(zm_log_level_t level, const char* format, ...) {
    if (!host_api_ || !host_api_->log) return;
    
    char buffer[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    
    host_api_->log(host_ctx_, level, buffer);
}

void StreamManager::log_stream(uint32_t stream_id, zm_log_level_t level, const char* format, ...) {
    if (!host_api_ || !host_api_->log) return;
    
    char msg_buffer[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(msg_buffer, sizeof(msg_buffer), format, args);
    va_end(args);
    
    char full_buffer[1024];
    snprintf(full_buffer, sizeof(full_buffer), "[Stream %u] %s", stream_id, msg_buffer);
    
    host_api_->log(host_ctx_, level, full_buffer);
}
