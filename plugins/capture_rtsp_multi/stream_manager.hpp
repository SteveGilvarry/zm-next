#pragma once

#include "zm_plugin.h"

#ifdef __cplusplus
extern "C" {
#endif
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/log.h>
#include <libavutil/hwcontext.h>
#include <libavutil/dict.h>
#include <libavutil/time.h>
#ifdef __cplusplus
}
#endif

#include <string>
#include <vector>
#include <map>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>
#include <memory>
#include <random>

// Configuration for a single RTSP stream
struct StreamConfig {
    std::string url;               // RTSP URL
    std::string transport;         // "tcp" or "udp"  
    uint32_t stream_id;           // Unique stream identifier
    bool hw_decode;               // Hardware decoding enabled
    int max_retry_attempts;       // Max reconnection attempts (-1 = infinite)
    int retry_delay_ms;           // Delay between retry attempts
    
    StreamConfig() : stream_id(0), hw_decode(false), max_retry_attempts(5), retry_delay_ms(2000) {}
};

// Runtime state for a single stream
struct StreamState {
    AVFormatContext* fmt_ctx;
    AVCodecContext* codec_ctx;
    AVPacket* packet;
    AVHWDeviceContext* hw_device_ctx;
    
    uint32_t stream_id;
    int video_stream_index;
    std::atomic<bool> running;
    std::atomic<bool> connected;
    std::thread capture_thread;
    
    // Connection/retry state
    int retry_count;
    int current_retry_delay_ms;
    std::chrono::steady_clock::time_point last_retry_time;
    std::chrono::steady_clock::time_point last_attempt_time;
    
    // Statistics
    uint64_t frames_captured;
    uint64_t packets_dropped;
    std::chrono::steady_clock::time_point start_time;
    
    StreamState() : fmt_ctx(nullptr), codec_ctx(nullptr), packet(nullptr), 
                   hw_device_ctx(nullptr), stream_id(0), video_stream_index(-1),
                   running(false), connected(false), retry_count(0), current_retry_delay_ms(1000),
                   frames_captured(0), packets_dropped(0) {}
};

// Multi-stream RTSP capture manager
class StreamManager {
public:
    StreamManager();
    ~StreamManager();
    
    // Initialize with host API and configuration
    bool initialize(zm_host_api_t* host_api, void* host_ctx, const char* json_config);
    
    // Stream lifecycle management
    bool start_all_streams();
    void stop_all_streams();
    bool add_stream(const StreamConfig& config);
    bool remove_stream(uint32_t stream_id);
    
    // Status and monitoring
    size_t get_stream_count() const;
    bool is_stream_connected(uint32_t stream_id) const;
    std::vector<uint32_t> get_active_stream_ids() const;
    
    // Statistics
    struct StreamStats {
        uint32_t stream_id;
        bool connected;
        uint64_t frames_captured;
        uint64_t packets_dropped;
        int retry_count;
        double uptime_seconds;
    };
    std::vector<StreamStats> get_stream_statistics() const;
    
private:
    // Host API for callbacks
    zm_host_api_t* host_api_;
    void* host_ctx_;
    
    // Stream management
    std::map<uint32_t, StreamConfig> stream_configs_;
    std::map<uint32_t, std::unique_ptr<StreamState>> stream_states_;
    mutable std::mutex streams_mutex_;
    
    // Global settings
    bool global_hw_decode_;
    std::string default_transport_;
    
    // FFmpeg hardware acceleration
    AVHWDeviceType preferred_hw_type_;
    
    // Reconnection constants
    static constexpr int MIN_RECONNECT_DELAY_MS = 1000;    // 1 second
    static constexpr int MAX_RECONNECT_DELAY_MS = 30000;   // 30 seconds
    
    // Random number generator for jitter
    mutable std::random_device rd_;
    mutable std::mt19937 gen_;
    mutable std::uniform_int_distribution<> jitter_dist_;
    
    // Helper methods
    bool parse_configuration(const char* json_config);
    bool setup_stream(uint32_t stream_id);
    void cleanup_stream(uint32_t stream_id);
    bool init_hardware_acceleration(StreamState* state, const AVCodec* codec);
    
    // Per-stream capture loop
    void capture_loop(uint32_t stream_id);
    bool connect_stream(StreamState* state, const StreamConfig& config);
    void handle_stream_disconnect(uint32_t stream_id);
    
    // Frame processing and publishing
    void process_and_publish_frame(StreamState* state, const StreamConfig& config);
    
    // Publishing and communication
    void log(zm_log_level_t level, const char* format, ...);
    void log_stream(uint32_t stream_id, zm_log_level_t level, const char* format, ...);
    void publish_event(const char* json_event);
    void publish_stream_metadata(uint32_t stream_id, const AVCodecParameters* codecpar);
};
