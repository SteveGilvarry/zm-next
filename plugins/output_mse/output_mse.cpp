#include <zm_plugin.h>
#include <mutex>
#include <queue>
#include <vector>
#include <atomic>
#include <condition_variable>
#include <unordered_map>
#include <memory>
#include <string>
#include <cstring>
#include <algorithm>
#include <nlohmann/json.hpp>
#include <boost/asio.hpp>
#include <boost/thread.hpp>
#include <boost/thread/shared_mutex.hpp>
#include <chrono>
#include <thread>
#include <mutex>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/mathematics.h>
}

using json = nlohmann::json;
using namespace boost::asio;
using boost::asio::ip::tcp;

// =============================================================================
// FORWARD DECLARATIONS
// =============================================================================

class MSEService;
class MSEControlServer;

// =============================================================================
// H.264 STREAM ANALYZER - Extract dimensions and codec info from stream
// =============================================================================

class H264StreamAnalyzer {
public:
    struct StreamInfo {
        int width = 0;
        int height = 0;
        int fps = 25;
        bool has_sps = false;
    };
    
    static bool analyzeSPS(const uint8_t* data, size_t size, StreamInfo& info);
    static bool findNALU(const uint8_t* data, size_t size, uint8_t nalu_type, const uint8_t** nalu_start, size_t* nalu_size);
    
private:
    static int parseUE(const uint8_t* data, int& bit_pos, int max_bits);
    static int parseSE(const uint8_t* data, int& bit_pos, int max_bits);
};

bool H264StreamAnalyzer::findNALU(const uint8_t* data, size_t size, uint8_t nalu_type, 
                                  const uint8_t** nalu_start, size_t* nalu_size) {
    for (size_t i = 0; i < size - 4; i++) {
        // Look for start code (0x00000001)
        if (data[i] == 0x00 && data[i+1] == 0x00 && data[i+2] == 0x00 && data[i+3] == 0x01) {
            if (i + 4 < size) {
                uint8_t found_type = data[i+4] & 0x1F;
                if (found_type == nalu_type) {
                    *nalu_start = &data[i+4];
                    // Find next start code or end of buffer
                    for (size_t j = i + 5; j < size - 3; j++) {
                        if (data[j] == 0x00 && data[j+1] == 0x00 && data[j+2] == 0x00 && data[j+3] == 0x01) {
                            *nalu_size = j - (i + 4);
                            return true;
                        }
                    }
                    *nalu_size = size - (i + 4);
                    return true;
                }
            }
        }
    }
    return false;
}

bool H264StreamAnalyzer::analyzeSPS(const uint8_t* data, size_t size, StreamInfo& info) {
    const uint8_t* sps_data;
    size_t sps_size;
    
    // Find SPS NALU (type 7)
    if (!findNALU(data, size, 7, &sps_data, &sps_size)) {
        return false;
    }
    
    if (sps_size < 10) return false; // Too small to be valid SPS
    
    // Simple SPS parsing for width/height
    // This is a basic implementation - for production you'd want more robust parsing
    try {
        int bit_pos = 8; // Skip NALU header
        
        // Skip profile_idc, constraint flags, level_idc
        bit_pos += 24;
        
        // Parse seq_parameter_set_id
        parseUE(sps_data, bit_pos, sps_size * 8);
        
        // Parse chroma_format_idc (if profile >= 100)
        uint8_t profile = sps_data[1];
        if (profile >= 100) {
            int chroma_format = parseUE(sps_data, bit_pos, sps_size * 8);
            if (chroma_format == 3) {
                bit_pos++; // separate_colour_plane_flag
            }
            parseUE(sps_data, bit_pos, sps_size * 8); // bit_depth_luma_minus8
            parseUE(sps_data, bit_pos, sps_size * 8); // bit_depth_chroma_minus8
            bit_pos++; // qpprime_y_zero_transform_bypass_flag
            
            // seq_scaling_matrix_present_flag
            if (bit_pos / 8 < sps_size && (sps_data[bit_pos / 8] & (1 << (7 - (bit_pos % 8))))) {
                bit_pos++; // Skip scaling matrix parsing for simplicity
                return false; // Too complex for basic parser
            } else {
                bit_pos++;
            }
        }
        
        // Parse log2_max_frame_num_minus4
        parseUE(sps_data, bit_pos, sps_size * 8);
        
        // Parse pic_order_cnt_type
        int poc_type = parseUE(sps_data, bit_pos, sps_size * 8);
        if (poc_type == 0) {
            parseUE(sps_data, bit_pos, sps_size * 8); // log2_max_pic_order_cnt_lsb_minus4
        } else if (poc_type == 1) {
            // Skip complex POC type 1 parsing
            return false;
        }
        
        // Parse max_num_ref_frames
        parseUE(sps_data, bit_pos, sps_size * 8);
        
        // Skip gaps_in_frame_num_value_allowed_flag
        bit_pos++;
        
        // Parse pic_width_in_mbs_minus1 and pic_height_in_map_units_minus1
        int width_mbs = parseUE(sps_data, bit_pos, sps_size * 8) + 1;
        int height_mbs = parseUE(sps_data, bit_pos, sps_size * 8) + 1;
        
        info.width = width_mbs * 16;
        info.height = height_mbs * 16;
        info.has_sps = true;
        
        zm_plugin_log_info("H264StreamAnalyzer: Detected dimensions %dx%d", info.width, info.height);
        return true;
        
    } catch (...) {
        zm_plugin_log_error("H264StreamAnalyzer: Error parsing SPS");
        return false;
    }
}

int H264StreamAnalyzer::parseUE(const uint8_t* data, int& bit_pos, int max_bits) {
    if (bit_pos >= max_bits) return 0;
    
    int leading_zeros = 0;
    while (bit_pos < max_bits && !(data[bit_pos / 8] & (1 << (7 - (bit_pos % 8))))) {
        leading_zeros++;
        bit_pos++;
    }
    
    if (bit_pos >= max_bits) return 0;
    
    bit_pos++; // Skip the '1' bit
    
    int value = 0;
    for (int i = 0; i < leading_zeros && bit_pos < max_bits; i++) {
        value = (value << 1) | ((data[bit_pos / 8] & (1 << (7 - (bit_pos % 8)))) ? 1 : 0);
        bit_pos++;
    }
    
    return (1 << leading_zeros) - 1 + value;
}

int H264StreamAnalyzer::parseSE(const uint8_t* data, int& bit_pos, int max_bits) {
    int ue = parseUE(data, bit_pos, max_bits);
    return (ue % 2) ? (ue + 1) / 2 : -(ue / 2);
}

// =============================================================================
// MSE PLUGIN IMPLEMENTATION WITH fMP4 FRAGMENTATION
// =============================================================================

// =============================================================================
// MP4 FRAGMENTER - H.264 to fMP4 conversion with keyframe boundaries
// =============================================================================

class MP4Fragmenter {
public:
    MP4Fragmenter();
    ~MP4Fragmenter();
    
    // Initialize for a specific stream
    bool initialize(uint32_t camera_id, int width, int height, int fps = 30);
    void cleanup();
    
    // Process H.264 frame and generate fMP4 segments when keyframes are detected
    // Returns true if a new segment was created
    bool processFrame(const uint8_t* h264_data, size_t size, bool is_keyframe, 
                     std::vector<uint8_t>& out_segment);
    
    // Force finish current segment (useful for stream end)
    bool finishSegment(std::vector<uint8_t>& out_segment);
    
    // Get initialization segment (init.mp4)
    bool getInitializationSegment(std::vector<uint8_t>& out_segment);
    
private:
    uint32_t camera_id_;
    AVFormatContext* fmt_ctx_;
    AVStream* video_stream_;
    AVIOContext* avio_ctx_;
    
    // Memory buffer for fMP4 output
    uint8_t* output_buffer_;
    size_t output_buffer_size_;
    size_t output_buffer_pos_;
    
    // Timing and frame tracking
    int64_t next_pts_;
    int64_t time_base_den_;
    uint32_t sequence_number_;
    bool initialized_;
    
    // Store initialization segment
    std::vector<uint8_t> init_segment_;
    bool init_segment_generated_;
    
    // Thread safety
    mutable std::mutex fragmenter_mutex_;
    
    // Helper methods
    bool setupMP4Context(int width, int height, int fps);
    bool writeInitializationSegment();
    static int writePacket(void* opaque, const uint8_t* buf, int buf_size);
    static int64_t seek(void* opaque, int64_t offset, int whence);
    void resetOutputBuffer();
    bool finalizeSegment(std::vector<uint8_t>& out_segment);
};

MP4Fragmenter::MP4Fragmenter() 
    : camera_id_(0), fmt_ctx_(nullptr), video_stream_(nullptr), avio_ctx_(nullptr),
      output_buffer_(nullptr), output_buffer_size_(0), output_buffer_pos_(0),
      next_pts_(0), time_base_den_(90000), sequence_number_(0), initialized_(false),
      init_segment_generated_(false) {
}

MP4Fragmenter::~MP4Fragmenter() {
    cleanup();
}

bool MP4Fragmenter::initialize(uint32_t camera_id, int width, int height, int fps) {
    std::lock_guard<std::mutex> lock(fragmenter_mutex_);
    
    if (initialized_) {
        cleanup();
    }
    
    camera_id_ = camera_id;
    time_base_den_ = fps * 1000; // Use millisecond precision
    
    // Store dimensions for later use
    if (width <= 0 || height <= 0) {
        zm_plugin_log_error("MP4Fragmenter: Invalid dimensions %dx%d", width, height);
        return false;
    }
    
    // Allocate output buffer (start with 1MB)
    output_buffer_size_ = 1024 * 1024;
    output_buffer_ = static_cast<uint8_t*>(av_malloc(output_buffer_size_));
    if (!output_buffer_) {
        zm_plugin_log_error("MP4Fragmenter: Failed to allocate output buffer");
        return false;
    }
    
    if (!setupMP4Context(width, height, fps)) {
        cleanup();
        return false;
    }
    
    initialized_ = true;
    zm_plugin_log_info("MP4Fragmenter: Initialized for camera %u (%dx%d, %dfps)", 
                       camera_id, width, height, fps);
    return true;
}

void MP4Fragmenter::cleanup() {
    if (fmt_ctx_) {
        if (initialized_ && fmt_ctx_->pb) {
            av_write_trailer(fmt_ctx_);
        }
        
        // Important: avformat_free_context will also free the avio_ctx if it's assigned to pb
        // So we need to clear our reference before freeing the format context
        if (fmt_ctx_->pb == avio_ctx_) {
            avio_ctx_ = nullptr; // Prevent double-free
        }
        
        avformat_free_context(fmt_ctx_);
        fmt_ctx_ = nullptr;
    }
    
    // Only free avio_ctx if it wasn't freed by avformat_free_context
    if (avio_ctx_) {
        av_freep(&avio_ctx_->buffer);
        av_freep(&avio_ctx_);
    }
    
    if (output_buffer_) {
        av_free(output_buffer_);
        output_buffer_ = nullptr;
    }
    
    output_buffer_size_ = 0;
    output_buffer_pos_ = 0;
    video_stream_ = nullptr;
    initialized_ = false;
}

bool MP4Fragmenter::setupMP4Context(int width, int height, int fps) {
    // Create format context for fragmented MP4
    int ret = avformat_alloc_output_context2(&fmt_ctx_, nullptr, "mp4", nullptr);
    if (ret < 0) {
        zm_plugin_log_error("MP4Fragmenter: Failed to create format context");
        return false;
    }
    
    // Enable fragmentation (use newer API approach)
    AVDictionary* options = nullptr;
    av_dict_set(&options, "movflags", "frag_keyframe+empty_moov", 0);
    
    // Create custom AVIO context for memory output
    const int avio_buffer_size = 32768;
    uint8_t* avio_buffer = static_cast<uint8_t*>(av_malloc(avio_buffer_size));
    if (!avio_buffer) {
        zm_plugin_log_error("MP4Fragmenter: Failed to allocate AVIO buffer");
        return false;
    }
    
    avio_ctx_ = avio_alloc_context(avio_buffer, avio_buffer_size, 1, this, 
                                   nullptr, writePacket, seek);
    if (!avio_ctx_) {
        av_free(avio_buffer);
        zm_plugin_log_error("MP4Fragmenter: Failed to create AVIO context");
        return false;
    }
    
    fmt_ctx_->pb = avio_ctx_;
    
    // Create video stream
    video_stream_ = avformat_new_stream(fmt_ctx_, nullptr);
    if (!video_stream_) {
        zm_plugin_log_error("MP4Fragmenter: Failed to create video stream");
        return false;
    }
    
    // Configure stream parameters for H.264
    video_stream_->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    video_stream_->codecpar->codec_id = AV_CODEC_ID_H264;
    video_stream_->codecpar->width = width;
    video_stream_->codecpar->height = height;
    video_stream_->codecpar->format = AV_PIX_FMT_YUV420P;
    video_stream_->codecpar->bit_rate = width * height * 2; // Reasonable bitrate estimate
    video_stream_->time_base = {1, static_cast<int>(time_base_den_)};
    
    // Write header
    resetOutputBuffer();
    ret = avformat_write_header(fmt_ctx_, &options);
    av_dict_free(&options);
    if (ret < 0) {
        zm_plugin_log_error("MP4Fragmenter: Failed to write header");
        return false;
    }
    
    // Generate and store initialization segment
    if (!writeInitializationSegment()) {
        zm_plugin_log_error("MP4Fragmenter: Failed to generate initialization segment");
        return false;
    }
    
    return true;
}

void MP4Fragmenter::resetOutputBuffer() {
    output_buffer_pos_ = 0;
}

int MP4Fragmenter::writePacket(void* opaque, const uint8_t* buf, int buf_size) {
    MP4Fragmenter* fragmenter = static_cast<MP4Fragmenter*>(opaque);
    
    // Ensure we have enough space
    if (fragmenter->output_buffer_pos_ + buf_size > fragmenter->output_buffer_size_) {
        size_t new_size = fragmenter->output_buffer_size_ * 2;
        while (new_size < fragmenter->output_buffer_pos_ + buf_size) {
            new_size *= 2;
        }
        
        uint8_t* new_buffer = static_cast<uint8_t*>(av_realloc(fragmenter->output_buffer_, new_size));
        if (!new_buffer) {
            zm_plugin_log_error("MP4Fragmenter: Failed to reallocate output buffer");
            return AVERROR(ENOMEM);
        }
        
        fragmenter->output_buffer_ = new_buffer;
        fragmenter->output_buffer_size_ = new_size;
    }
    
    // Copy data to output buffer
    memcpy(fragmenter->output_buffer_ + fragmenter->output_buffer_pos_, buf, buf_size);
    fragmenter->output_buffer_pos_ += buf_size;
    
    return buf_size;
}

int64_t MP4Fragmenter::seek(void* opaque, int64_t offset, int whence) {
    MP4Fragmenter* fragmenter = static_cast<MP4Fragmenter*>(opaque);
    
    switch (whence) {
        case SEEK_SET:
            fragmenter->output_buffer_pos_ = offset;
            break;
        case SEEK_CUR:
            fragmenter->output_buffer_pos_ += offset;
            break;
        case SEEK_END:
            fragmenter->output_buffer_pos_ = fragmenter->output_buffer_size_ + offset;
            break;
        case AVSEEK_SIZE:
            return fragmenter->output_buffer_size_;
        default:
            return AVERROR(EINVAL);
    }
    
    return fragmenter->output_buffer_pos_;
}

bool MP4Fragmenter::processFrame(const uint8_t* h264_data, size_t size, bool is_keyframe, 
                                std::vector<uint8_t>& out_segment) {
    std::lock_guard<std::mutex> lock(fragmenter_mutex_);
    
    if (!initialized_ || !h264_data || size == 0) {
        return false;
    }
    
    // Create packet for this frame
    AVPacket* pkt = av_packet_alloc();
    if (!pkt) {
        zm_plugin_log_error("MP4Fragmenter: Failed to allocate packet");
        return false;
    }
    
    // Allocate packet buffer and copy H.264 data
    int ret = av_new_packet(pkt, size);
    if (ret < 0) {
        zm_plugin_log_error("MP4Fragmenter: Failed to allocate packet buffer");
        av_packet_free(&pkt);
        return false;
    }
    std::memcpy(pkt->data, h264_data, size);
    
    pkt->stream_index = video_stream_->index;
    pkt->pts = next_pts_;
    pkt->dts = next_pts_;
    pkt->duration = time_base_den_ / 30; // Assume 30fps for now
    
    if (is_keyframe) {
        pkt->flags |= AV_PKT_FLAG_KEY;
    }
    
    // Reset output buffer for new segment
    if (is_keyframe && sequence_number_ > 0) {
        // This will start a new fragment on keyframe
        resetOutputBuffer();
    }
    
    // Write the packet
    int64_t duration = pkt->duration; // Save duration before freeing packet
    int write_ret = av_interleaved_write_frame(fmt_ctx_, pkt);
    av_packet_free(&pkt);
    
    if (write_ret < 0) {
        zm_plugin_log_error("MP4Fragmenter: Failed to write frame");
        return false;
    }
    
    next_pts_ += duration;
    
    // If this is a keyframe and we have data, finalize the segment
    if (is_keyframe && output_buffer_pos_ > 0) {
        return finalizeSegment(out_segment);
    }
    
    return false; // No segment ready yet
}

bool MP4Fragmenter::finishSegment(std::vector<uint8_t>& out_segment) {
    std::lock_guard<std::mutex> lock(fragmenter_mutex_);
    
    if (!initialized_) {
        return false;
    }
    
    // Flush any remaining frames
    av_interleaved_write_frame(fmt_ctx_, nullptr);
    
    return finalizeSegment(out_segment);
}

bool MP4Fragmenter::finalizeSegment(std::vector<uint8_t>& out_segment) {
    if (output_buffer_pos_ == 0) {
        return false;
    }
    
    // Copy buffer to output segment
    out_segment.assign(output_buffer_, output_buffer_ + output_buffer_pos_);
    
    sequence_number_++;
    
    zm_plugin_log_debug("MP4Fragmenter: Generated fMP4 segment %u, size %zu bytes", 
                        sequence_number_, out_segment.size());
    
    return true;
}

bool MP4Fragmenter::writeInitializationSegment() {
    // The initialization segment is the header we just wrote
    if (output_buffer_pos_ > 0) {
        init_segment_.assign(output_buffer_, output_buffer_ + output_buffer_pos_);
        init_segment_generated_ = true;
        zm_plugin_log_info("MP4Fragmenter: Generated initialization segment, size %zu bytes", init_segment_.size());
        
        // Reset buffer for regular segments
        resetOutputBuffer();
        return true;
    }
    return false;
}

bool MP4Fragmenter::getInitializationSegment(std::vector<uint8_t>& out_segment) {
    std::lock_guard<std::mutex> lock(fragmenter_mutex_);
    
    if (!init_segment_generated_ || init_segment_.empty()) {
        return false;
    }
    
    out_segment = init_segment_;
    return true;
}

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
    
    // Pop next segment, blocks if empty
    std::vector<uint8_t> pop() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [&]{ return !queue_.empty(); });
        auto seg = queue_.front();
        queue_.pop();
        return seg;
    }
    
    // Non-blocking get
    bool try_pop(std::vector<uint8_t>& out) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) return false;
        out = queue_.front();
        queue_.pop();
        return true;
    }
    
    // Get buffer stats
    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }
    
    uint64_t get_dropped_count() const { return dropped_segments_; }
    uint64_t get_total_count() const { return total_segments_; }
    
    // Get latest segment without removing it (for API access)
    bool get_latest(std::vector<uint8_t>& out) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) return false;
        out = queue_.back(); // Get most recent segment
        return true;
    }
    
private:
    mutable std::mutex mutex_;
    std::queue<std::vector<uint8_t>> queue_;
    std::condition_variable cv_;
    const size_t max_segments_ = 100;
    std::atomic<uint64_t> dropped_segments_{0};
    std::atomic<uint64_t> total_segments_{0};
};

// =============================================================================
// STREAM INFO
// =============================================================================

struct StreamInfo {
    uint32_t camera_id;
    uint32_t stream_id;
    std::string codec;
    int width = 0;
    int height = 0;
    int fps = 25;
    bool dimensions_detected = false;
    std::unique_ptr<MSESegmentBuffer> buffer;
    std::unique_ptr<MP4Fragmenter> fragmenter;
    std::atomic<uint64_t> frame_count{0};
    std::atomic<uint64_t> bytes_received{0};
    
    // Track frames processed without SPS for fallback
    uint32_t frames_without_sps = 0;
    
    StreamInfo(uint32_t cam_id, uint32_t str_id, const std::string& c) 
        : camera_id(cam_id), stream_id(str_id), codec(c), 
          buffer(std::make_unique<MSESegmentBuffer>()),
          fragmenter(std::make_unique<MP4Fragmenter>()) {}
};

// =============================================================================
// MSE CONTROL SERVER
// =============================================================================

class MSEControlServer {
public:
    MSEControlServer(io_context& io_ctx, class MSEService& service);
    
    void start(const std::string& address, uint16_t port);
    void stop();
    
private:
    void accept_connections();
    void handle_client(std::shared_ptr<tcp::socket> socket);
    void process_command(const json& cmd, std::shared_ptr<tcp::socket> socket);
    
    io_context& io_context_;
    class MSEService& mse_service_;
    tcp::acceptor acceptor_;
    std::atomic<bool> running_{false};
};

// =============================================================================
// MSE SERVICE (SINGLETON)
// =============================================================================

class MSEService {
public:
    static MSEService& getInstance() {
        static MSEService instance;
        return instance;
    }
    
    bool initialize();
    void shutdown();
    
    // Control server management
    void startControlServer(const std::string& bind_address, uint16_t port);
    void stopControlServer();
    
    void registerStream(uint32_t camera_id, uint32_t stream_id, const std::string& codec, int width = 0, int height = 0);
    void unregisterStream(uint32_t camera_id, uint32_t stream_id);
    void pushSegment(uint32_t camera_id, const uint8_t* data, size_t size);
    
    // C API functions
    size_t popSegment(uint32_t camera_id, uint8_t* out, size_t max_size);
    size_t tryPopSegment(uint32_t camera_id, uint8_t* out, size_t max_size);
    size_t getBufferSize(uint32_t camera_id);
    size_t getBufferStats(uint32_t camera_id, uint64_t* total_segments_received, uint64_t* dropped_segments);
    uint64_t getBytesReceived(uint32_t camera_id);
    uint64_t getFrameCount(uint32_t camera_id);
    
    // Initialization segment access
    size_t getInitializationSegment(uint32_t camera_id, uint8_t* out, size_t max_size);
    
    // Latest segment access
    size_t getLatestSegment(uint32_t camera_id, uint8_t* out, size_t max_size);
    
    // Debug function to list active cameras
    size_t getActiveCameras(uint32_t* camera_ids, size_t max_cameras);
    
    // Helper method for auto-registration
    bool ensureStreamRegistered(uint32_t camera_id);
    
    // Statistics for IPC
    json getStatistics() const;
    
private:
    MSEService() = default;
    ~MSEService() = default;
    MSEService(const MSEService&) = delete;
    MSEService& operator=(const MSEService&) = delete;
    
    mutable std::mutex streams_mutex_;
    std::unordered_map<std::string, std::unique_ptr<StreamInfo>> streams_;
    std::atomic<bool> running_{false};
    
    // Control server
    std::unique_ptr<MSEControlServer> control_server_;
    std::unique_ptr<io_context> io_context_;
    std::unique_ptr<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> work_guard_;
    std::unique_ptr<std::thread> io_thread_;
    
    std::string getStreamKey(uint32_t camera_id, uint32_t stream_id) {
        return std::to_string(camera_id) + ":" + std::to_string(stream_id);
    }
    
    StreamInfo* findStreamByCamera(uint32_t camera_id);
};

bool MSEService::initialize() {
    if (running_) return true;
    
    // Initialize IO context for control server
    io_context_ = std::make_unique<io_context>();
    
    // Create work guard to keep IO context alive
    work_guard_ = std::make_unique<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>(
        boost::asio::make_work_guard(*io_context_));
    
    // Start IO context thread
    io_thread_ = std::make_unique<std::thread>([this]() {
        io_context_->run();
    });
    
    running_ = true;
    zm_plugin_log_info("MSE Service initialized");
    return true;
}

void MSEService::shutdown() {
    running_ = false;
    
    if (control_server_) {
        control_server_->stop();
    }
    
    // Release work guard to allow IO context to exit
    if (work_guard_) {
        work_guard_.reset();
    }
    
    if (io_context_) {
        io_context_->stop();
    }
    
    if (io_thread_ && io_thread_->joinable()) {
        io_thread_->join();
    }
    
    std::lock_guard<std::mutex> lock(streams_mutex_);
    streams_.clear();
    zm_plugin_log_info("MSE Service shutdown");
}

void MSEService::startControlServer(const std::string& bind_address, uint16_t port) {
    if (!control_server_ && io_context_) {
        control_server_ = std::make_unique<MSEControlServer>(*io_context_, *this);
        control_server_->start(bind_address, port);
        zm_plugin_log_info("MSE Control Server started on %s:%d", bind_address.c_str(), port);
    }
}

void MSEService::stopControlServer() {
    if (control_server_) {
        control_server_->stop();
        control_server_.reset();
        zm_plugin_log_info("MSE Control Server stopped");
    }
}

void MSEService::registerStream(uint32_t camera_id, uint32_t stream_id, const std::string& codec, int width, int height) {
    std::string stream_key = getStreamKey(camera_id, stream_id);
    
    zm_plugin_log_debug("MSE: registerStream called - camera_id=%u, stream_id=%u, codec=%s, key=%s", 
                        camera_id, stream_id, codec.c_str(), stream_key.c_str());
    
    std::lock_guard<std::mutex> lock(streams_mutex_);
    auto stream_info = std::make_unique<StreamInfo>(camera_id, stream_id, codec);
    stream_info->width = width;
    stream_info->height = height;
    
    // Initialize MP4 fragmenter for H.264 streams
    if (codec == "h264" || codec == "H264") {
        if (!stream_info->fragmenter->initialize(camera_id, width, height)) {
            zm_plugin_log_error("MSE: Failed to initialize MP4 fragmenter for stream %u:%u", 
                      camera_id, stream_id);
            return;
        }
    }
    
    streams_[stream_key] = std::move(stream_info);
    zm_plugin_log_info("MSE: Registered stream %u:%u (%s, %dx%d)", 
                       camera_id, stream_id, codec.c_str(), width, height);
}

void MSEService::unregisterStream(uint32_t camera_id, uint32_t stream_id) {
    std::string stream_key = getStreamKey(camera_id, stream_id);
    
    std::lock_guard<std::mutex> lock(streams_mutex_);
    streams_.erase(stream_key);
    zm_plugin_log_info("MSE: Unregistered stream %u:%u", camera_id, stream_id);
}

StreamInfo* MSEService::findStreamByCamera(uint32_t camera_id) {
    // Find first stream for this camera (most common case: 1 stream per camera)
    for (const auto& [stream_key, stream_info] : streams_) {
        if (stream_info->camera_id == camera_id) {
            return stream_info.get();
        }
    }
    return nullptr;
}

void MSEService::pushSegment(uint32_t camera_id, const uint8_t* data, size_t size) {
    if (!running_ || !data || size == 0) return;
    
    zm_plugin_log_debug("MSE: pushSegment called for camera %u, size %zu", camera_id, size);
    
    std::lock_guard<std::mutex> lock(streams_mutex_);
    auto* stream = findStreamByCamera(camera_id);
    if (!stream) {
        zm_plugin_log_error("MSE: pushSegment - camera %u not found! Registered cameras:", camera_id);
        for (const auto& [stream_key, stream_info] : streams_) {
            zm_plugin_log_error("MSE: - Camera %u (key: %s)", stream_info->camera_id, stream_key.c_str());
        }
        return;
    }
    
    stream->frame_count++;
    stream->bytes_received += size;
    
    // For H.264 streams, analyze dimensions if not yet detected
    if ((stream->codec == "h264" || stream->codec == "H264") && !stream->dimensions_detected) {
        // Debug: Check what NALU types we're receiving
        if (size >= 5) {
            for (size_t i = 0; i < size - 4; i++) {
                if (data[i] == 0x00 && data[i+1] == 0x00 && data[i+2] == 0x00 && data[i+3] == 0x01) {
                    uint8_t nalu_type = data[i+4] & 0x1F;
                    zm_plugin_log_debug("MSE: Found NALU type %u in frame (size %zu)", nalu_type, size);
                    if (nalu_type == 7) { // SPS
                        zm_plugin_log_info("MSE: Found SPS NALU in frame, attempting dimension detection");
                    }
                    break; // Just check first NALU for debugging
                }
            }
        }
        
        H264StreamAnalyzer::StreamInfo stream_info;
        if (H264StreamAnalyzer::analyzeSPS(data, size, stream_info)) {
            stream->width = stream_info.width;
            stream->height = stream_info.height;
            stream->fps = stream_info.fps;
            stream->dimensions_detected = true;
            
            // Initialize MP4 fragmenter with detected dimensions (first time only)
            if (!stream->fragmenter->initialize(camera_id, stream->width, stream->height, stream->fps)) {
                zm_plugin_log_error("MSE: Failed to initialize MP4 fragmenter with detected dimensions %dx%d", 
                                   stream->width, stream->height);
                return;
            }
            
            zm_plugin_log_info("MSE: Initialized MP4 fragmenter for camera %u with detected dimensions %dx%d@%dfps", 
                               camera_id, stream->width, stream->height, stream->fps);
        } else {
            stream->frames_without_sps++;
            zm_plugin_log_debug("MSE: Waiting for SPS to detect dimensions for camera %u (frame %u)", 
                               camera_id, stream->frames_without_sps);
            
            // Fallback: if we haven't detected SPS after 10 frames, use the registered dimensions
            if (stream->frames_without_sps >= 10 && stream->width > 0 && stream->height > 0) {
                zm_plugin_log_info("MSE: Using fallback dimensions %dx%d for camera %u after %u frames without SPS",
                                   stream->width, stream->height, camera_id, stream->frames_without_sps);
                stream->dimensions_detected = true;
                
                if (!stream->fragmenter->initialize(camera_id, stream->width, stream->height, stream->fps)) {
                    zm_plugin_log_error("MSE: Failed to initialize MP4 fragmenter with fallback dimensions %dx%d", 
                                       stream->width, stream->height);
                    return;
                }
                
                zm_plugin_log_info("MSE: Initialized MP4 fragmenter for camera %u with fallback dimensions %dx%d@%dfps", 
                                   camera_id, stream->width, stream->height, stream->fps);
            } else {
                // Don't process frames until we have proper dimensions
                return;
            }
        }
    }
    
    // Only process frames if we have initialized the fragmenter
    if ((stream->codec == "h264" || stream->codec == "H264") && stream->fragmenter && stream->dimensions_detected) {
        // Simple keyframe detection for H.264: look for I-frame NALU (type 5)
        bool is_keyframe = false;
        if (size >= 5) {
            // Look for H.264 start code (0x00000001) followed by NALU header
            for (size_t i = 0; i < size - 4; i++) {
                if (data[i] == 0x00 && data[i+1] == 0x00 && data[i+2] == 0x00 && data[i+3] == 0x01) {
                    uint8_t nalu_type = data[i+4] & 0x1F;
                    if (nalu_type == 5) { // IDR slice
                        is_keyframe = true;
                        break;
                    }
                }
            }
        }
        
        std::vector<uint8_t> fmp4_segment;
        if (stream->fragmenter->processFrame(data, size, is_keyframe, fmp4_segment)) {
            // We got a complete fMP4 segment, push it to the buffer
            stream->buffer->push(fmp4_segment);
            zm_plugin_log_debug("MSE: Generated fMP4 segment for camera %u, size %zu bytes", 
                               camera_id, fmp4_segment.size());
        }
    } else if (stream->codec != "h264" && stream->codec != "H264") {
        // For other codecs or raw data, push as-is
        std::vector<uint8_t> segment(data, data + size);
        stream->buffer->push(segment);
        zm_plugin_log_debug("MSE: Pushed raw segment for camera %u, size %zu bytes", 
                           camera_id, size);
    } else {
        zm_plugin_log_debug("MSE: Waiting for SPS to detect dimensions for camera %u", camera_id);
    }
}

size_t MSEService::popSegment(uint32_t camera_id, uint8_t* out, size_t max_size) {
    StreamInfo* stream = nullptr;
    {
        std::lock_guard<std::mutex> lock(streams_mutex_);
        stream = findStreamByCamera(camera_id);
        if (!stream) return 0;
    }
    
    // Now we can safely call the blocking pop without holding the streams lock
    auto segment = stream->buffer->pop();
    
    size_t to_copy = std::min(static_cast<size_t>(max_size), segment.size());
    std::memcpy(out, segment.data(), to_copy);
    return to_copy;
}

size_t MSEService::tryPopSegment(uint32_t camera_id, uint8_t* out, size_t max_size) {
    std::lock_guard<std::mutex> lock(streams_mutex_);
    auto* stream = findStreamByCamera(camera_id);
    if (!stream) return 0;
    
    std::vector<uint8_t> segment;
    if (!stream->buffer->try_pop(segment)) return 0;
    
    size_t to_copy = std::min(static_cast<size_t>(max_size), segment.size());
    std::memcpy(out, segment.data(), to_copy);
    return to_copy;
}

size_t MSEService::getBufferSize(uint32_t camera_id) {
    std::lock_guard<std::mutex> lock(streams_mutex_);
    auto* stream = findStreamByCamera(camera_id);
    if (!stream) {
        zm_plugin_log_debug("MSE: getBufferSize - camera %u not found", camera_id);
        return 0;
    }
    size_t size = stream->buffer->size();
    zm_plugin_log_debug("MSE: getBufferSize - camera %u has %zu segments", camera_id, size);
    return size;
}

size_t MSEService::getBufferStats(uint32_t camera_id, uint64_t* total_segments_received, uint64_t* dropped_segments) {
    std::lock_guard<std::mutex> lock(streams_mutex_);
    auto* stream = findStreamByCamera(camera_id);
    if (!stream) {
        if (total_segments_received) *total_segments_received = 0;
        if (dropped_segments) *dropped_segments = 0;
        return 0;
    }
    
    if (total_segments_received) *total_segments_received = stream->buffer->get_total_count();
    if (dropped_segments) *dropped_segments = stream->buffer->get_dropped_count();
    return stream->buffer->size();
}

uint64_t MSEService::getBytesReceived(uint32_t camera_id) {
    std::lock_guard<std::mutex> lock(streams_mutex_);
    auto* stream = findStreamByCamera(camera_id);
    return stream ? stream->bytes_received.load() : 0;
}

uint64_t MSEService::getFrameCount(uint32_t camera_id) {
    std::lock_guard<std::mutex> lock(streams_mutex_);
    auto* stream = findStreamByCamera(camera_id);
    return stream ? stream->frame_count.load() : 0;
}

size_t MSEService::getInitializationSegment(uint32_t camera_id, uint8_t* out, size_t max_size) {
    std::lock_guard<std::mutex> lock(streams_mutex_);
    auto* stream = findStreamByCamera(camera_id);
    if (!stream || !stream->fragmenter) return 0;
    
    // Copy initialization segment data
    std::vector<uint8_t> init_segment;
    if (!stream->fragmenter->getInitializationSegment(init_segment)) return 0;
    
    size_t to_copy = std::min(max_size, init_segment.size());
    std::memcpy(out, init_segment.data(), to_copy);
    return to_copy;
}

size_t MSEService::getLatestSegment(uint32_t camera_id, uint8_t* out, size_t max_size) {
    std::lock_guard<std::mutex> lock(streams_mutex_);
    auto* stream = findStreamByCamera(camera_id);
    if (!stream || !stream->buffer) return 0;
    
    std::vector<uint8_t> latest_segment;
    if (!stream->buffer->get_latest(latest_segment)) return 0;
    
    size_t to_copy = std::min(max_size, latest_segment.size());
    std::memcpy(out, latest_segment.data(), to_copy);
    return to_copy;
}

size_t MSEService::getActiveCameras(uint32_t* camera_ids, size_t max_cameras) {
    std::lock_guard<std::mutex> lock(streams_mutex_);
    size_t count = 0;
    
    for (const auto& pair : streams_) {
        if (count >= max_cameras) break;
        camera_ids[count] = pair.second->camera_id;
        count++;
    }
    
    return count;
}

bool MSEService::ensureStreamRegistered(uint32_t camera_id) {
    std::lock_guard<std::mutex> lock(streams_mutex_);
    auto* stream = findStreamByCamera(camera_id);
    if (!stream) {
        // Auto-register stream as H.264 with temporary dimensions
        // Real dimensions will be detected from H.264 SPS later
        zm_plugin_log_info("MSE: Auto-registering H.264 stream for camera %u (720p)", camera_id);
        
        // Create the stream info directly since we're already holding the lock
        std::string stream_key = getStreamKey(camera_id, 0);
        auto stream_info = std::make_unique<StreamInfo>(camera_id, 0, "h264");
        stream_info->width = 1280;
        stream_info->height = 720;
        stream_info->dimensions_detected = false; // Will be updated when SPS is detected
        
        // Don't initialize MP4 fragmenter yet - wait for real dimensions from SPS
        // This prevents double initialization and potential memory issues
        
        streams_[stream_key] = std::move(stream_info);
        zm_plugin_log_info("MSE: Successfully registered stream %u:0 (h264, 1280x720)", camera_id);
        return true;
    }
    return true; // Already registered
}

json MSEService::getStatistics() const {
    json stats;
    
    std::lock_guard<std::mutex> lock(streams_mutex_);
    
    stats["total_streams"] = streams_.size();
    
    json streams_stats = json::array();
    for (const auto& [stream_key, stream] : streams_) {
        json stream_stat;
        stream_stat["camera_id"] = stream->camera_id;
        stream_stat["stream_id"] = stream->stream_id;
        stream_stat["codec"] = stream->codec;
        stream_stat["width"] = stream->width;
        stream_stat["height"] = stream->height;
        stream_stat["frame_count"] = stream->frame_count.load();
        stream_stat["bytes_received"] = stream->bytes_received.load();
        stream_stat["dimensions_detected"] = stream->dimensions_detected;
        
        if (stream->buffer) {
            stream_stat["buffer_size"] = stream->buffer->size();
            stream_stat["total_segments"] = stream->buffer->get_total_count();
            stream_stat["dropped_segments"] = stream->buffer->get_dropped_count();
        }
        
        streams_stats.push_back(stream_stat);
    }
    stats["streams"] = streams_stats;
    
    return stats;
}

// =============================================================================
// MSE CONTROL SERVER IMPLEMENTATION
// =============================================================================

MSEControlServer::MSEControlServer(io_context& io_ctx, MSEService& service) 
    : io_context_(io_ctx), mse_service_(service), acceptor_(io_ctx) {}

void MSEControlServer::start(const std::string& address, uint16_t port) {
    try {
        tcp::endpoint endpoint(boost::asio::ip::make_address(address), port);
        acceptor_.open(endpoint.protocol());
        acceptor_.set_option(tcp::acceptor::reuse_address(true));
        acceptor_.bind(endpoint);
        acceptor_.listen();
        
        running_ = true;
        zm_plugin_log_info("MSE Control server listening on %s:%d", address.c_str(), port);
        
        accept_connections();
    } catch (const std::exception& e) {
        zm_plugin_log_error("Failed to start MSE control server: %s", e.what());
    }
}

void MSEControlServer::stop() {
    running_ = false;
    if (acceptor_.is_open()) {
        acceptor_.close();
    }
}

void MSEControlServer::accept_connections() {
    auto socket = std::make_shared<tcp::socket>(io_context_);
    
    acceptor_.async_accept(*socket,
        [this, socket](boost::system::error_code ec) {
            if (!ec && running_) {
                zm_plugin_log_debug("MSE Control Server: New connection accepted");
                std::thread([this, socket]() {
                    handle_client(socket);
                }).detach();
                
                accept_connections();
            } else if (ec && running_) {
                zm_plugin_log_error("MSE Control Server: Accept error: %s", ec.message().c_str());
                if (running_) {
                    accept_connections();
                }
            }
        });
}

void MSEControlServer::handle_client(std::shared_ptr<tcp::socket> socket) {
    std::string client_endpoint = "unknown";
    try {
        auto remote_endpoint = socket->remote_endpoint();
        client_endpoint = remote_endpoint.address().to_string() + ":" + std::to_string(remote_endpoint.port());
    } catch (const std::exception& e) {
        zm_plugin_log_debug("MSE Control Server: Unable to get client endpoint: %s", e.what());
    }
    
    try {
        boost::asio::streambuf buffer;
        
        while (running_ && socket->is_open()) {
            boost::system::error_code ec;
            size_t bytes = boost::asio::read_until(*socket, buffer, '\n', ec);
            
            if (ec) {
                if (ec != boost::asio::error::eof) {
                    zm_plugin_log_debug("MSE Control Server: Read error from %s: %s", client_endpoint.c_str(), ec.message().c_str());
                }
                break;
            }
            
            std::istream is(&buffer);
            std::string line;
            std::getline(is, line);
            
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            
            try {
                json cmd = json::parse(line);
                process_command(cmd, socket);
            } catch (const json::exception& e) {
                zm_plugin_log_error("MSE Control Server: Invalid JSON command from %s: %s", client_endpoint.c_str(), e.what());
                json error_response = {{"error", "Invalid JSON"}};
                std::string response = error_response.dump() + "\n";
                socket->write_some(boost::asio::buffer(response));
            }
        }
    } catch (const std::exception& e) {
        zm_plugin_log_error("MSE Control Server: Client handling error for %s: %s", client_endpoint.c_str(), e.what());
    }
}

void MSEControlServer::process_command(const json& cmd, std::shared_ptr<tcp::socket> socket) {
    try {
        std::string command = cmd.at("command");
        uint32_t camera_id = cmd.value("camera_id", 0);
        
        json response;
        response["command"] = command;
        response["camera_id"] = camera_id;
        
        if (command == "get_init_segment") {
            std::vector<uint8_t> buffer(64 * 1024); // 64KB should be enough for init segment
            size_t size = mse_service_.getInitializationSegment(camera_id, buffer.data(), buffer.size());
            
            if (size > 0) {
                response["success"] = true;
                response["size"] = size;
                response["type"] = "binary_follows";
                
                // Send JSON response first
                std::string response_str = response.dump() + "\n";
                socket->write_some(boost::asio::buffer(response_str));
                
                // Then send binary data
                socket->write_some(boost::asio::buffer(buffer.data(), size));
                return; // Don't send response again at the end
            } else {
                response["success"] = false;
                response["error"] = "No initialization segment available";
            }
            
        } else if (command == "get_latest_segment") {
            std::vector<uint8_t> buffer(256 * 1024); // 256KB should be enough for media segment
            size_t size = mse_service_.getLatestSegment(camera_id, buffer.data(), buffer.size());
            
            if (size > 0) {
                response["success"] = true;
                response["size"] = size;
                response["type"] = "binary_follows";
                
                // Send JSON response first
                std::string response_str = response.dump() + "\n";
                socket->write_some(boost::asio::buffer(response_str));
                
                // Then send binary data
                socket->write_some(boost::asio::buffer(buffer.data(), size));
                return; // Don't send response again at the end
            } else {
                response["success"] = false;
                response["error"] = "No segment available";
            }
            
        } else if (command == "get_buffer_stats") {
            uint64_t total_segments = 0, dropped_segments = 0;
            size_t buffer_size = mse_service_.getBufferStats(camera_id, &total_segments, &dropped_segments);
            
            response["success"] = true;
            response["buffer_size"] = buffer_size;
            response["total_segments"] = total_segments;
            response["dropped_segments"] = dropped_segments;
            response["bytes_received"] = mse_service_.getBytesReceived(camera_id);
            response["frame_count"] = mse_service_.getFrameCount(camera_id);
            
        } else if (command == "get_active_cameras") {
            std::vector<uint32_t> camera_ids(32); // Support up to 32 cameras
            size_t count = mse_service_.getActiveCameras(camera_ids.data(), camera_ids.size());
            
            json cameras = json::array();
            for (size_t i = 0; i < count; i++) {
                cameras.push_back(camera_ids[i]);
            }
            
            response["success"] = true;
            response["cameras"] = cameras;
            response["count"] = count;
            
        } else if (command == "get_stats") {
            response = mse_service_.getStatistics();
            response["success"] = true;
            
        } else {
            response["error"] = "Unknown command";
            response["success"] = false;
        }
        
        std::string response_str = response.dump() + "\n";
        socket->write_some(boost::asio::buffer(response_str));
        
    } catch (const std::exception& e) {
        zm_plugin_log_error("MSE Control Server: Command processing error: %s", e.what());
        json error_response = {
            {"error", e.what()},
            {"success", false}
        };
        std::string response = error_response.dump() + "\n";
        socket->write_some(boost::asio::buffer(response));
    }
}

// Global service instance
static MSEService& g_mse_service = MSEService::getInstance();

// =============================================================================
// PLUGIN IMPLEMENTATION
// =============================================================================

struct OutputMSEPlugin {
    // Store host API for C ABI functions to use
    zm_host_api_t* host_api;
    void* host_ctx;
    
    // Configuration
    uint32_t camera_id;
    uint32_t stream_id; 
    std::string codec;
};

// Parse JSON configuration
bool parse_config(const char* json_cfg, OutputMSEPlugin* plugin) {
    if (!json_cfg || !plugin) return false;
    
    // Simple JSON parsing for camera_id (basic implementation)
    // Look for "camera_id": <number>
    const char* camera_id_key = "\"camera_id\"";
    const char* camera_id_pos = strstr(json_cfg, camera_id_key);
    if (camera_id_pos) {
        // Find the number after the colon
        const char* colon = strchr(camera_id_pos, ':');
        if (colon) {
            plugin->camera_id = (uint32_t)strtoul(colon + 1, nullptr, 10);
        }
    }
    
    // Default values
    if (plugin->camera_id == 0) plugin->camera_id = 1;
    plugin->stream_id = 0; // Default
    plugin->codec = "h264"; // Default
    
    zm_plugin_log_info("MSE: Parsed config - camera_id=%u, stream_id=%u, codec=%s", 
                       plugin->camera_id, plugin->stream_id, plugin->codec.c_str());
    return true;
}

static int mse_start(zm_plugin_t* plugin, zm_host_api_t* host, void* host_ctx, const char* json_cfg) {
    if (!plugin || !host) return -1;
    
    // Set up logging context
    zm_plugin_set_log_context(host, host_ctx);
    
    // Initialize FFmpeg (required for MP4 fragmentation)
    zm_plugin_log_info("MSE: Initializing FFmpeg libraries");
    
    // Initialize the MSE service
    if (!g_mse_service.initialize()) {
        zm_plugin_log_error("Failed to initialize MSE service");
        return -1;
    }
    
    // Start control server (once, like WebRTC plugin)
    static std::once_flag server_started;
    std::call_once(server_started, []() {
        g_mse_service.startControlServer("127.0.0.1", 9051);
    });
    
    // Create plugin instance and store host API
    auto* plugin_instance = new OutputMSEPlugin();
    plugin_instance->host_api = host;
    plugin_instance->host_ctx = host_ctx;
    
    // Parse configuration
    if (!parse_config(json_cfg, plugin_instance)) {
        zm_plugin_log_error("MSE: Failed to parse configuration");
        delete plugin_instance;
        return -1;
    }
    
    plugin->instance = plugin_instance;
    
    zm_plugin_log_info("MSE Output Plugin started with fMP4 fragmentation support and control server on port 9051");
    return 0;
}

static void mse_stop(zm_plugin_t* plugin) {
    if (!plugin || !plugin->instance) return;
    
    // Shutdown MSE service
    g_mse_service.shutdown();
    
    // Clean up plugin instance
    delete static_cast<OutputMSEPlugin*>(plugin->instance);
    plugin->instance = nullptr;
    
    zm_plugin_log_info("MSE Output Plugin stopped");
}

static void mse_on_frame(zm_plugin_t* plugin, const void* buf, size_t size) {
    if (!plugin || !plugin->instance || !buf || size < sizeof(zm_frame_hdr_t)) return;
    
    OutputMSEPlugin* plugin_instance = static_cast<OutputMSEPlugin*>(plugin->instance);
    const zm_frame_hdr_t* hdr = static_cast<const zm_frame_hdr_t*>(buf);
    const uint8_t* payload = static_cast<const uint8_t*>(buf) + sizeof(zm_frame_hdr_t);
    size_t payload_size = size - sizeof(zm_frame_hdr_t);
    
    // Use the configured camera_id from the plugin configuration
    uint32_t camera_id = plugin_instance->camera_id;
    
    zm_plugin_log_debug("MSE: Processing frame for configured camera %u (stream_id in frame: %u)", 
                        camera_id, hdr->stream_id);
    
    if (payload_size > 0) {
        // Ensure stream is registered
        if (!g_mse_service.ensureStreamRegistered(camera_id)) {
            zm_plugin_log_error("MSE: Failed to register stream for camera %u", camera_id);
            return;
        }
        
        // Push the frame data for MP4 fragmentation
        g_mse_service.pushSegment(camera_id, payload, payload_size);
        
        zm_plugin_log_debug("MSE: Processed frame for camera %u, size %zu bytes, pts %lu", 
                           camera_id, payload_size, hdr->pts_usec);
    }
}

// =============================================================================
// COMBINED C API (MSE API + PLUGIN INTERFACE)
// =============================================================================

extern "C" {

// Plugin initialization function
void zm_plugin_init(zm_plugin_t* plugin) {
    if (!plugin) return;
    
    plugin->version = 1;
    plugin->type = ZM_PLUGIN_OUTPUT;
    plugin->start = mse_start;
    plugin->stop = mse_stop;
    plugin->on_frame = mse_on_frame;
    plugin->instance = nullptr;
}

// Cleanup function (optional)
void cleanup_plugin(zm_plugin_t* plugin) {
    if (plugin && plugin->instance) {
        delete static_cast<OutputMSEPlugin*>(plugin->instance);
        plugin->instance = nullptr;
    }
}

} // end extern "C"
