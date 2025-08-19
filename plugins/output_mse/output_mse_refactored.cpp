#include <zm_plugin.h>
#include <zm_plugin_utils.h>
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <unordered_map>
#include <atomic>
#include <string>
#include <cstring>

// =============================================================================
// MSE SEGMENT BUFFER
// =============================================================================

// Thread-safe buffer for MSE segments per stream
class MSESegmentBuffer {
public:
    void push(const std::vector<uint8_t>& segment) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.size() > max_segments_) {
            queue_.pop(); // drop oldest
            dropped_segments_++;
        }
        queue_.push(segment);
        total_segments_++;
        cv_.notify_all();
    }

    bool pop(std::vector<uint8_t>& segment) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) return false;
        
        segment = queue_.front();
        queue_.pop();
        return true;
    }

    bool wait_and_pop(std::vector<uint8_t>& segment, int timeout_ms = 1000) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                        [this] { return !queue_.empty(); })) {
            segment = queue_.front();
            queue_.pop();
            return true;
        }
        return false;
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    void get_stats(uint64_t& total, uint64_t& dropped, size_t& current) const {
        std::lock_guard<std::mutex> lock(mutex_);
        total = total_segments_;
        dropped = dropped_segments_;
        current = queue_.size();
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<std::vector<uint8_t>> queue_;
    size_t max_segments_ = 100;
    std::atomic<uint64_t> total_segments_{0};
    std::atomic<uint64_t> dropped_segments_{0};
};

// =============================================================================
// STREAM INFORMATION
// =============================================================================

struct StreamInfo {
    uint32_t camera_id;
    uint32_t stream_id;
    std::string codec;
    MSESegmentBuffer buffer;
    std::atomic<uint64_t> frame_count{0};
    std::atomic<uint64_t> bytes_received{0};
    
    StreamInfo(uint32_t cam_id, uint32_t str_id, const std::string& c)
        : camera_id(cam_id), stream_id(str_id), codec(c) {}
};

// =============================================================================
// PLUGIN STATE MANAGEMENT
// =============================================================================

class MSEPluginManager {
public:
    static MSEPluginManager& getInstance() {
        static MSEPluginManager instance;
        return instance;
    }

    void registerStream(uint32_t camera_id, uint32_t stream_id, const std::string& codec) {
        std::lock_guard<std::mutex> lock(streams_mutex_);
        std::string key = std::to_string(camera_id) + ":" + std::to_string(stream_id);
        
        if (streams_.find(key) != streams_.end()) {
            ZM_LOG_WARN("Stream %u:%u already registered, replacing", camera_id, stream_id);
        }
        
        streams_[key] = std::make_shared<StreamInfo>(camera_id, stream_id, codec);
        ZM_LOG_INFO("Registered MSE stream %u:%u (%s)", camera_id, stream_id, codec.c_str());
    }

    void unregisterStream(uint32_t camera_id, uint32_t stream_id) {
        std::lock_guard<std::mutex> lock(streams_mutex_);
        std::string key = std::to_string(camera_id) + ":" + std::to_string(stream_id);
        
        auto it = streams_.find(key);
        if (it != streams_.end()) {
            streams_.erase(it);
            ZM_LOG_INFO("Unregistered MSE stream %u:%u", camera_id, stream_id);
        }
    }

    void pushFrame(const zm_frame_hdr_t* frame_hdr, const uint8_t* frame_data, size_t data_size) {
        std::shared_lock<std::shared_mutex> lock(streams_mutex_);
        
        // Find stream by stream_id
        std::shared_ptr<StreamInfo> stream = nullptr;
        for (const auto& [key, info] : streams_) {
            if (info->stream_id == frame_hdr->stream_id) {
                stream = info;
                break;
            }
        }
        
        if (!stream) {
            ZM_LOG_ERROR("Frame received for unknown stream %u", frame_hdr->stream_id);
            return;
        }
        
        // Create MSE segment (simplified - in practice would do proper MP4 fragmentation)
        std::vector<uint8_t> segment(frame_data, frame_data + data_size);
        stream->buffer.push(segment);
        
        stream->frame_count++;
        stream->bytes_received += data_size;
        
        // Throttled logging to avoid spam
        zm_plugin_log_debug_throttled(5, "MSE: Processed frame for stream %u:%u, size=%zu", 
                                     stream->camera_id, stream->stream_id, data_size);
    }

    std::shared_ptr<StreamInfo> getStream(uint32_t camera_id) {
        std::shared_lock<std::shared_mutex> lock(streams_mutex_);
        for (const auto& [key, info] : streams_) {
            if (info->camera_id == camera_id) {
                return info;
            }
        }
        return nullptr;
    }

    void publishStatistics() {
        std::shared_lock<std::shared_mutex> lock(streams_mutex_);
        
        uint64_t total_frames = 0;
        uint64_t total_bytes = 0;
        
        for (const auto& [key, stream] : streams_) {
            total_frames += stream->frame_count;
            total_bytes += stream->bytes_received;
            
            uint64_t total_segments, dropped_segments;
            size_t current_segments;
            stream->buffer.get_stats(total_segments, dropped_segments, current_segments);
            
            // Publish per-stream stats
            char stream_stats[512];
            snprintf(stream_stats, sizeof(stream_stats),
                    "{\"camera_id\":%u,\"stream_id\":%u,\"codec\":\"%s\","
                    "\"frames\":%llu,\"bytes\":%llu,\"segments\":%llu,"
                    "\"dropped_segments\":%llu,\"queue_size\":%zu}",
                    stream->camera_id, stream->stream_id, stream->codec.c_str(),
                    (unsigned long long)stream->frame_count.load(),
                    (unsigned long long)stream->bytes_received.load(),
                    (unsigned long long)total_segments,
                    (unsigned long long)dropped_segments,
                    current_segments);
            
            zm_plugin_publish_event("mse_stream_stats", stream_stats);
        }
        
        // Publish overall plugin stats
        zm_plugin_stats_t overall_stats = {0};
        overall_stats.frames_processed = total_frames;
        overall_stats.bytes_processed = total_bytes;
        overall_stats.plugin_name = "output_mse";
        overall_stats.plugin_version = "1.0.0";
        
        zm_plugin_publish_stats(&overall_stats);
    }

private:
    mutable std::shared_mutex streams_mutex_;
    std::unordered_map<std::string, std::shared_ptr<StreamInfo>> streams_;
};

// =============================================================================
// C API IMPLEMENTATION
// =============================================================================

extern "C" {

int mse_get_segment(uint32_t camera_id, uint8_t* buffer, size_t buffer_size, size_t* segment_size) {
    auto stream = MSEPluginManager::getInstance().getStream(camera_id);
    if (!stream) {
        ZM_LOG_ERROR("No MSE stream found for camera %u", camera_id);
        return -1;
    }
    
    std::vector<uint8_t> segment;
    if (!stream->buffer.pop(segment)) {
        return 0; // No data available
    }
    
    if (segment.size() > buffer_size) {
        ZM_LOG_ERROR("Segment size %zu exceeds buffer size %zu", segment.size(), buffer_size);
        return -1;
    }
    
    memcpy(buffer, segment.data(), segment.size());
    *segment_size = segment.size();
    
    ZM_LOG_DEBUG("Retrieved MSE segment for camera %u, size=%zu", camera_id, segment.size());
    return 1; // Success
}

int mse_wait_for_segment(uint32_t camera_id, uint8_t* buffer, size_t buffer_size, 
                        size_t* segment_size, int timeout_ms) {
    auto stream = MSEPluginManager::getInstance().getStream(camera_id);
    if (!stream) {
        ZM_LOG_ERROR("No MSE stream found for camera %u", camera_id);
        return -1;
    }
    
    std::vector<uint8_t> segment;
    if (!stream->buffer.wait_and_pop(segment, timeout_ms)) {
        return 0; // Timeout
    }
    
    if (segment.size() > buffer_size) {
        ZM_LOG_ERROR("Segment size %zu exceeds buffer size %zu", segment.size(), buffer_size);
        return -1;
    }
    
    memcpy(buffer, segment.data(), segment.size());
    *segment_size = segment.size();
    
    ZM_LOG_DEBUG("Retrieved MSE segment for camera %u after wait, size=%zu", camera_id, segment.size());
    return 1; // Success
}

size_t mse_get_queue_size(uint32_t camera_id) {
    auto stream = MSEPluginManager::getInstance().getStream(camera_id);
    if (!stream) {
        ZM_LOG_ERROR("No MSE stream found for camera %u", camera_id);
        return 0;
    }
    
    return stream->buffer.size();
}

} // extern "C"

// =============================================================================
// PLUGIN LIFECYCLE FUNCTIONS
// =============================================================================

struct MSEPluginInstance {
    uint32_t camera_id;
    uint32_t stream_id;
    std::string codec;
    bool initialized = false;
    
    MSEPluginInstance() = default;
    MSEPluginInstance(uint32_t cam_id, uint32_t str_id, const std::string& codec_type)
        : camera_id(cam_id), stream_id(str_id), codec(codec_type), initialized(true) {}
    
    ~MSEPluginInstance() {
        if (initialized) {
            MSEPluginManager::getInstance().unregisterStream(camera_id, stream_id);
        }
    }
};

static void* mse_init(const char* config_json) {
    try {
        auto instance = std::make_unique<MSEPluginInstance>();
        
        if (config_json) {
            // Parse JSON config here (simplified)
            instance->camera_id = 1; // Default values
            instance->stream_id = 1;
            instance->codec = "h264";
        }
        
        instance->initialized = true;
        ZM_LOG_INFO("MSE plugin initialized for camera %u, stream %u", 
                   instance->camera_id, instance->stream_id);
        
        return instance.release();
        
    } catch (const std::exception& e) {
        ZM_LOG_ERROR("Failed to initialize MSE plugin: %s", e.what());
        return nullptr;
    }
}

static void mse_shutdown(void* instance_ptr) {
    if (instance_ptr) {
        auto instance = static_cast<MSEPluginInstance*>(instance_ptr);
        delete instance;
        ZM_LOG_INFO("MSE plugin shutdown");
    }
}

static int mse_start(zm_plugin_t* plugin, zm_host_api_t* host, void* host_ctx, const char* json_cfg) {
    if (!plugin || !host || !json_cfg) return -1;
    
    // Initialize logging context using the new utilities
    ZmPluginLogger logger(host, host_ctx);
    
    try {
        // Parse configuration and register stream
        auto instance = static_cast<MSEPluginInstance*>(plugin->instance);
        if (!instance) {
            ZM_LOG_ERROR("Invalid plugin instance");
            return -1;
        }
        
        MSEPluginManager::getInstance().registerStream(
            instance->camera_id, instance->stream_id, instance->codec);
        
        ZM_LOG_INFO("MSE plugin started successfully");
        return 0;
        
    } catch (const std::exception& e) {
        ZM_LOG_ERROR("Failed to start MSE plugin: %s", e.what());
        return -1;
    }
}

static int mse_stop(zm_plugin_t* plugin) {
    if (!plugin) return -1;
    
    auto instance = static_cast<MSEPluginInstance*>(plugin->instance);
    if (instance && instance->initialized) {
        MSEPluginManager::getInstance().unregisterStream(
            instance->camera_id, instance->stream_id);
    }
    
    ZM_LOG_INFO("MSE plugin stopped");
    return 0;
}

static int mse_process_frame(zm_plugin_t* plugin, const void* frame_data, size_t frame_size) {
    if (!plugin || !frame_data || frame_size < sizeof(zm_frame_hdr_t)) {
        ZM_LOG_ERROR("Invalid frame data");
        return -1;
    }
    
    const zm_frame_hdr_t* frame_hdr = static_cast<const zm_frame_hdr_t*>(frame_data);
    const uint8_t* payload = static_cast<const uint8_t*>(frame_data) + sizeof(zm_frame_hdr_t);
    size_t payload_size = frame_size - sizeof(zm_frame_hdr_t);
    
    MSEPluginManager::getInstance().pushFrame(frame_hdr, payload, payload_size);
    return 0;
}

// =============================================================================
// PLUGIN REGISTRATION
// =============================================================================

extern "C" {

int zm_plugin_init(zm_plugin_t* plugin) {
    if (!plugin) return -1;
    
    plugin->api_version = ZM_PLUGIN_API_VERSION;
    plugin->type = ZM_PLUGIN_OUTPUT;
    snprintf(plugin->name, sizeof(plugin->name), "output_mse");
    snprintf(plugin->version, sizeof(plugin->version), "1.0.0");
    snprintf(plugin->description, sizeof(plugin->description), 
             "MSE Output Plugin with improved logging");
    
    plugin->init = mse_init;
    plugin->shutdown = mse_shutdown;
    plugin->start = mse_start;
    plugin->stop = mse_stop;
    plugin->process_frame = mse_process_frame;
    
    return 0;
}

} // extern "C"
