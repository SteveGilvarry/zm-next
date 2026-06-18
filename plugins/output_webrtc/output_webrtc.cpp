#include <zm_plugin.h>
#include <nlohmann/json.hpp>
#include <rtc/rtc.hpp>
#include <boost/asio.hpp>
#include <boost/thread.hpp>
#include <boost/thread/shared_mutex.hpp>
#include <string>
#include <memory>
#include <unordered_map>
#include <queue>
#include <atomic>
#include <chrono>
#include <fstream>
#include <sstream>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <cstring>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/time.h>
}

using json = nlohmann::json;
using namespace boost::asio;
using boost::asio::ip::tcp;

// =============================================================================
// LOGGING HELPER
// =============================================================================

// Global host API pointer for logging (set during plugin start)
static zm_host_api_t* g_host_api = nullptr;
static void* g_host_ctx = nullptr;

static void log_info(const char* format, ...) {
    if (!g_host_api || !g_host_api->log) return;
    
    va_list args;
    va_start(args, format);
    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    g_host_api->log(g_host_ctx, ZM_LOG_INFO, buffer);
}

static void log_error(const char* format, ...) {
    if (!g_host_api || !g_host_api->log) return;
    
    va_list args;
    va_start(args, format);
    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    g_host_api->log(g_host_ctx, ZM_LOG_ERROR, buffer);
}

static void log_debug(const char* format, ...) {
    if (!g_host_api || !g_host_api->log) return;
    
    va_list args;
    va_start(args, format);
    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    g_host_api->log(g_host_ctx, ZM_LOG_DEBUG, buffer);
}

// =============================================================================
// FORWARD DECLARATIONS
// =============================================================================

class WebRTCService;
class PeerConnectionManager;
class ControlServer;

// =============================================================================
// DATA STRUCTURES
// =============================================================================

struct StreamInfo {
    uint32_t camera_id;
    uint32_t stream_id;
    std::string codec;
    int width = 0;
    int height = 0;
    AVCodecParameters* codec_params = nullptr;
    std::atomic<uint64_t> frame_count{0};
    std::atomic<uint64_t> bytes_sent{0};
    
    StreamInfo(uint32_t cam_id, uint32_t str_id, const std::string& c) 
        : camera_id(cam_id), stream_id(str_id), codec(c) {}
    
    ~StreamInfo() {
        if (codec_params) {
            avcodec_parameters_free(&codec_params);
        }
    }
};

struct ViewerSession {
    std::string viewer_id;
    uint32_t camera_id;
    std::shared_ptr<rtc::PeerConnection> peer_connection;
    std::shared_ptr<rtc::Track> video_track;
    std::chrono::steady_clock::time_point last_activity;
    std::atomic<bool> is_connected{false};
    uint32_t ssrc;
    uint16_t sequence_number = 0;
    
    ViewerSession(const std::string& id, uint32_t cam_id, uint32_t ssrc_val)
        : viewer_id(id), camera_id(cam_id), ssrc(ssrc_val), 
          last_activity(std::chrono::steady_clock::now()) {}
};

struct FrameData {
    std::vector<uint8_t> data;
    uint64_t timestamp;
    bool is_keyframe;
    uint32_t camera_id;
    uint32_t stream_id;
    
    FrameData(const uint8_t* frame_data, size_t size, uint64_t ts, bool keyframe, 
              uint32_t cam_id, uint32_t str_id)
        : data(frame_data, frame_data + size), timestamp(ts), is_keyframe(keyframe),
          camera_id(cam_id), stream_id(str_id) {}
};

// =============================================================================
// PEER CONNECTION MANAGER
// =============================================================================

class PeerConnectionManager {
public:
    PeerConnectionManager(std::shared_ptr<rtc::Configuration> config) : rtc_config_(config) {}
    
    std::shared_ptr<rtc::PeerConnection> createPeerConnection(const std::string& viewer_id);
    void addVideoTrack(std::shared_ptr<rtc::PeerConnection> pc, uint32_t ssrc);
    void sendFrame(const std::string& viewer_key, const FrameData& frame);
    
private:
    std::shared_ptr<rtc::Configuration> rtc_config_;
    std::unordered_map<std::string, std::shared_ptr<rtc::Track>> tracks_;
    mutable std::mutex tracks_mutex_;
};

std::shared_ptr<rtc::PeerConnection> PeerConnectionManager::createPeerConnection(const std::string& viewer_id) {
    auto pc = std::make_shared<rtc::PeerConnection>(*rtc_config_);
    
    pc->onStateChange([viewer_id](rtc::PeerConnection::State state) {
        // Only log significant state changes to reduce verbosity
        if (state == rtc::PeerConnection::State::Connected || 
            state == rtc::PeerConnection::State::Failed ||
            state == rtc::PeerConnection::State::Disconnected) {
            log_info("Viewer %s connection state: %d", viewer_id.c_str(), static_cast<int>(state));
        }
    });
    
    pc->onDataChannel([](std::shared_ptr<rtc::DataChannel> dc) {
        // DataChannel opened
    });
    
    return pc;
}

void PeerConnectionManager::addVideoTrack(std::shared_ptr<rtc::PeerConnection> pc, uint32_t ssrc) {
    rtc::Description::Video video("video", rtc::Description::Direction::SendOnly);
    video.addH264Codec(96);
    video.addSSRC(ssrc, "video-stream");
    
    auto track = pc->addTrack(video);
    
    // Create H.264 RTP packetizer for this track
    auto rtpConfig = std::make_shared<rtc::RtpPacketizationConfig>(ssrc, "video", 96, rtc::H264RtpPacketizer::defaultClockRate);
    auto h264_packetizer = std::make_shared<rtc::H264RtpPacketizer>(rtc::NalUnit::Separator::StartSequence, rtpConfig);
    track->setMediaHandler(h264_packetizer);
    
    std::lock_guard<std::mutex> lock(tracks_mutex_);
    tracks_[std::to_string(ssrc)] = track;
}

void PeerConnectionManager::sendFrame(const std::string& viewer_key, const FrameData& frame) {
    std::lock_guard<std::mutex> lock(tracks_mutex_);
    auto it = tracks_.find(viewer_key);
    if (it != tracks_.end() && it->second) {
        try {
            // Convert uint8_t data to std::byte for libdatachannel
            rtc::binary nal_data;
            nal_data.reserve(frame.data.size());
            for (uint8_t byte : frame.data) {
                nal_data.push_back(static_cast<std::byte>(byte));
            }
            
            // Send H.264 NAL unit - timestamps will be managed by RTP packetizer
            it->second->send(nal_data);
            
        } catch (const std::exception& e) {
            // Filter out "Track is closed" errors as they're expected during disconnection
            std::string error_msg = e.what();
            if (error_msg.find("Track is closed") == std::string::npos) {
                log_error("Failed to send frame: %s", e.what());
            }
            // Note: Track closed errors are normal during viewer disconnection
        }
    }
}

// =============================================================================
// CONTROL SERVER
// =============================================================================

class ControlServer {
public:
    ControlServer(io_context& io_ctx, class WebRTCService& service);
    
    void start(const std::string& address, uint16_t port);
    void stop();
    
private:
    void accept_connections();
    void handle_client(std::shared_ptr<tcp::socket> socket);
    void process_command(const json& cmd, std::shared_ptr<tcp::socket> socket);
    
    io_context& io_context_;
    class WebRTCService& webrtc_service_;
    tcp::acceptor acceptor_;
    std::atomic<bool> running_{false};
};

// =============================================================================
// WEBRTC SERVICE (SINGLETON)
// =============================================================================

class WebRTCService {
public:
    static WebRTCService& getInstance() {
        static WebRTCService instance;
        return instance;
    }
    
    void initialize();
    void shutdown();
    
    // Stream management
    void registerStream(uint32_t camera_id, uint32_t stream_id, const std::string& codec, 
                       AVCodecParameters* codec_params);
    void unregisterStream(uint32_t camera_id, uint32_t stream_id);
    void pushFrame(const zm_frame_hdr_t* frame_hdr, const uint8_t* frame_data, size_t data_size);
    
    // Viewer management
    std::string createOffer(uint32_t camera_id, const std::string& viewer_id);
    bool setAnswer(uint32_t camera_id, const std::string& viewer_id, const std::string& answer);
    bool addIceCandidate(uint32_t camera_id, const std::string& viewer_id, 
                        const std::string& candidate, const std::string& sdp_mid);
    bool dropViewer(uint32_t camera_id, const std::string& viewer_id);
    
    // Control server
    void startControlServer(const std::string& bind_address, uint16_t port);
    void stopControlServer();
    
    // Statistics
    json getStatistics() const;
    
private:
    WebRTCService() = default;
    ~WebRTCService() = default;
    WebRTCService(const WebRTCService&) = delete;
    WebRTCService& operator=(const WebRTCService&) = delete;
    
    void processFrameQueue();
    void cleanupStaleConnections();
    std::shared_ptr<ViewerSession> createViewerSession(uint32_t camera_id, const std::string& viewer_id);
    
    std::unique_ptr<PeerConnectionManager> peer_manager_;
    std::unique_ptr<ControlServer> control_server_;
    std::unique_ptr<io_context> io_context_;
    std::unique_ptr<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> work_guard_;
    
    // Stream management
    std::unordered_map<std::string, std::shared_ptr<StreamInfo>> streams_; // "camera_id:stream_id" -> StreamInfo
    mutable boost::shared_mutex streams_mutex_;
    
    // Viewer management
    std::unordered_map<std::string, std::shared_ptr<ViewerSession>> viewers_; // "camera_id:viewer_id" -> ViewerSession
    mutable boost::shared_mutex viewers_mutex_;
    
    // Frame processing
    std::queue<std::shared_ptr<FrameData>> frame_queue_;
    mutable std::mutex frame_queue_mutex_;
    std::condition_variable frame_queue_cv_;
    
    // Threading
    std::unique_ptr<std::thread> frame_processor_thread_;
    std::unique_ptr<std::thread> cleanup_thread_;
    std::unique_ptr<std::thread> io_thread_;
    std::atomic<bool> running_{false};
    
    // Statistics
    std::atomic<uint64_t> total_frames_processed_{0};
    std::atomic<uint64_t> total_bytes_processed_{0};
    std::atomic<uint64_t> total_connections_created_{0};
    std::atomic<uint64_t> total_connections_dropped_{0};
    
    // Configuration
    std::shared_ptr<rtc::Configuration> rtc_config_;
    std::atomic<uint32_t> next_ssrc_{1000};
};

// =============================================================================
// CONTROL SERVER IMPLEMENTATION
// =============================================================================

ControlServer::ControlServer(io_context& io_ctx, WebRTCService& service) 
    : io_context_(io_ctx), webrtc_service_(service), acceptor_(io_ctx) {}

void ControlServer::start(const std::string& address, uint16_t port) {
    try {
        tcp::endpoint endpoint(boost::asio::ip::make_address(address), port);
        acceptor_.open(endpoint.protocol());
        acceptor_.set_option(tcp::acceptor::reuse_address(true));
        acceptor_.bind(endpoint);
        acceptor_.listen();
        
        running_ = true;
        log_info("Control server listening on %s:%d", address.c_str(), port);
        
        accept_connections();
    } catch (const std::exception& e) {
        log_error("Failed to start control server: %s", e.what());
    }
}

void ControlServer::stop() {
    running_ = false;
    if (acceptor_.is_open()) {
        acceptor_.close();
    }
}

void ControlServer::accept_connections() {
    auto socket = std::make_shared<tcp::socket>(io_context_);
    
    acceptor_.async_accept(*socket,
        [this, socket](boost::system::error_code ec) {
            if (!ec && running_) {
                log_info("WebRTC Control Server: New connection accepted, starting client handler thread");
                std::thread([this, socket]() {
                    handle_client(socket);
                }).detach();
                
                accept_connections();
            } else if (ec && running_) {
                log_info("WebRTC Control Server: Accept error: %s", ec.message().c_str());
                // Continue accepting connections unless server is shutting down
                if (running_) {
                    accept_connections();
                }
            }
        });
}

void ControlServer::handle_client(std::shared_ptr<tcp::socket> socket) {
    // Get client endpoint information for logging
    std::string client_endpoint = "unknown";
    try {
        auto remote_endpoint = socket->remote_endpoint();
        client_endpoint = remote_endpoint.address().to_string() + ":" + std::to_string(remote_endpoint.port());
        // Reduce verbosity for new connections
        // log_info("WebRTC Control Server: Client connected from %s", client_endpoint.c_str());
    } catch (const std::exception& e) {
        log_info("WebRTC Control Server: Unable to get client endpoint: %s", e.what());
    }
    
    try {
        boost::asio::streambuf buffer;
        
        while (running_ && socket->is_open()) {
            boost::system::error_code ec;
            size_t bytes = boost::asio::read_until(*socket, buffer, '\n', ec);
            
            if (ec) {
                if (ec != boost::asio::error::eof) {
            // log_info only for errors, not normal read operations
            if (ec != boost::asio::error::eof) {
                log_info("WebRTC Control Server: Read error from %s: %s", client_endpoint.c_str(), ec.message().c_str());
            }
                }
                break;
            }
            
            std::istream is(&buffer);
            std::string line;
            std::getline(is, line);
            
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            
            // Remove verbose command logging - keep only for debugging
            // log_info("WebRTC Control Server: Received command from %s: %s", client_endpoint.c_str(), line.c_str());
            
            try {
                json cmd = json::parse(line);
                process_command(cmd, socket);
            } catch (const json::exception& e) {
                log_error("WebRTC Control Server: Invalid JSON command from %s: %s", client_endpoint.c_str(), e.what());
                json error_response = {{"error", "Invalid JSON"}};
                std::string response = error_response.dump() + "\n";
                socket->write_some(boost::asio::buffer(response));
            }
        }
    } catch (const std::exception& e) {
        log_error("WebRTC Control Server: Client handling error for %s: %s", client_endpoint.c_str(), e.what());
    }
    
    // Reduce verbosity for disconnections
    // log_info("WebRTC Control Server: Client %s disconnected", client_endpoint.c_str());
}

void ControlServer::process_command(const json& cmd, std::shared_ptr<tcp::socket> socket) {
    try {
        std::string command = cmd.at("command");
        uint32_t camera_id = cmd.at("camera_id");
        std::string viewer_id = cmd.at("viewer_id");
        
        // Reduce verbosity - log only important operations
        // log_info("WebRTC Control Server: Processing command '%s' for camera %u, viewer %s", 
        //          command.c_str(), camera_id, viewer_id.c_str());
        
        json response;
        response["command"] = command;
        response["camera_id"] = camera_id;
        response["viewer_id"] = viewer_id;
        
        if (command == "create_offer") {
            std::string offer = webrtc_service_.createOffer(camera_id, viewer_id);
            response["offer"] = offer;
            response["success"] = !offer.empty();
            // Reduce logging verbosity
            // log_info("WebRTC Control Server: Created offer for camera %u, viewer %s (success: %s, offer size: %zu)", 
            //          camera_id, viewer_id.c_str(), response["success"].get<bool>() ? "true" : "false", offer.length());
            
        } else if (command == "set_answer") {
            std::string answer = cmd.at("answer");
            bool success = webrtc_service_.setAnswer(camera_id, viewer_id, answer);
            response["success"] = success;
            // log_info("WebRTC Control Server: Set answer for camera %u, viewer %s (success: %s, answer size: %zu)", 
            //          camera_id, viewer_id.c_str(), success ? "true" : "false", answer.length());
            
        } else if (command == "add_ice_candidate") {
            std::string candidate = cmd.at("candidate");
            std::string sdp_mid = cmd.value("sdp_mid", "");
            bool success = webrtc_service_.addIceCandidate(camera_id, viewer_id, candidate, sdp_mid);
            response["success"] = success;
            // log_info("WebRTC Control Server: Added ICE candidate for camera %u, viewer %s (success: %s, sdp_mid: %s)", 
            //          camera_id, viewer_id.c_str(), success ? "true" : "false", sdp_mid.c_str());
            
        } else if (command == "drop_viewer") {
            bool success = webrtc_service_.dropViewer(camera_id, viewer_id);
            response["success"] = success;
            // log_info("WebRTC Control Server: Dropped viewer %s for camera %u (success: %s)", 
            //          viewer_id.c_str(), camera_id, success ? "true" : "false");
            
        } else if (command == "get_stats") {
            response = webrtc_service_.getStatistics();
            response["success"] = true;
            // log_info("WebRTC Control Server: Retrieved statistics (response size: %zu bytes)", 
            //          response.dump().length());
            
        } else {
            response["error"] = "Unknown command";
            response["success"] = false;
            // log_info("WebRTC Control Server: Unknown command '%s' for camera %u, viewer %s", 
            //          command.c_str(), camera_id, viewer_id.c_str());
        }
        
        std::string response_str = response.dump() + "\n";
        socket->write_some(boost::asio::buffer(response_str));
        // log_info("WebRTC Control Server: Sent response for command '%s' (size: %zu bytes)", 
        //          command.c_str(), response_str.length());
        
    } catch (const std::exception& e) {
        log_error("WebRTC Control Server: Command processing error: %s", e.what());
        json error_response = {
            {"error", e.what()},
            {"success", false}
        };
        std::string response = error_response.dump() + "\n";
        socket->write_some(boost::asio::buffer(response));
        // log_info("WebRTC Control Server: Sent error response (size: %zu bytes)", response.length());
    }
}

// =============================================================================
// WEBRTC SERVICE IMPLEMENTATION
// =============================================================================

void WebRTCService::initialize() {
    if (running_) return;
    
    // Initialize WebRTC configuration
    rtc_config_ = std::make_shared<rtc::Configuration>();
    rtc_config_->iceServers.emplace_back("stun:stun.l.google.com:19302");
    
    // Initialize peer connection manager
    peer_manager_ = std::make_unique<PeerConnectionManager>(rtc_config_);
    
    // Initialize IO context
    io_context_ = std::make_unique<io_context>();
    
    // Create work guard to keep IO context alive
    work_guard_ = std::make_unique<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>(
        boost::asio::make_work_guard(*io_context_));
    
    // Start processing threads
    running_ = true;
    
    frame_processor_thread_ = std::make_unique<std::thread>([this]() {
        processFrameQueue();
    });
    
    cleanup_thread_ = std::make_unique<std::thread>([this]() {
        while (running_) {
            std::this_thread::sleep_for(std::chrono::seconds(30));
            cleanupStaleConnections();
        }
    });
    
    // Start IO context thread AFTER work guard is created
    io_thread_ = std::make_unique<std::thread>([this]() {
        io_context_->run();
    });
    
    log_info("WebRTC Service initialized");
}

void WebRTCService::shutdown() {
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
    
    frame_queue_cv_.notify_all();
    
    if (frame_processor_thread_ && frame_processor_thread_->joinable()) {
        frame_processor_thread_->join();
    }
    
    if (cleanup_thread_ && cleanup_thread_->joinable()) {
        cleanup_thread_->join();
    }
    
    if (io_thread_ && io_thread_->joinable()) {
        io_thread_->join();
    }
    
    {
        boost::unique_lock<boost::shared_mutex> lock(viewers_mutex_);
        viewers_.clear();
    }
    
    {
        boost::unique_lock<boost::shared_mutex> lock(streams_mutex_);
        streams_.clear();
    }
    
    log_info("WebRTC Service shutdown");
}

void WebRTCService::registerStream(uint32_t camera_id, uint32_t stream_id, const std::string& codec, 
                                  AVCodecParameters* codec_params) {
    std::string stream_key = std::to_string(camera_id) + ":" + std::to_string(stream_id);
    
    boost::unique_lock<boost::shared_mutex> lock(streams_mutex_);
    
    auto stream_info = std::make_shared<StreamInfo>(camera_id, stream_id, codec);
    if (codec_params) {
        stream_info->codec_params = avcodec_parameters_alloc();
        avcodec_parameters_copy(stream_info->codec_params, codec_params);
        stream_info->width = codec_params->width;
        stream_info->height = codec_params->height;
    }
    
    streams_[stream_key] = stream_info;
    
    log_info("Registered stream %u:%u (%s, %dx%d)", 
           camera_id, stream_id, codec.c_str(), stream_info->width, stream_info->height);
}

void WebRTCService::unregisterStream(uint32_t camera_id, uint32_t stream_id) {
    std::string stream_key = std::to_string(camera_id) + ":" + std::to_string(stream_id);
    
    boost::unique_lock<boost::shared_mutex> lock(streams_mutex_);
    streams_.erase(stream_key);
    
    log_info("Unregistered stream %u:%u", camera_id, stream_id);
}

void WebRTCService::pushFrame(const zm_frame_hdr_t* frame_hdr, const uint8_t* frame_data, size_t data_size) {
    if (!running_ || !frame_hdr || !frame_data || data_size == 0) return;
    
    uint32_t stream_id = frame_hdr->stream_id;
    bool is_keyframe = (frame_hdr->flags & 1) != 0;
    
    // Minimal logging for keyframes only and frame rate monitoring
    static uint64_t frame_count = 0;
    static auto last_rate_check = std::chrono::steady_clock::now();
    frame_count++;
    
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_rate_check);
    if (elapsed.count() >= 5) {  // Check frame rate every 5 seconds
        double fps = static_cast<double>(frame_count) / elapsed.count();
        log_info("WebRTC: Receiving %.1f fps for stream %u (total frames: %llu)", fps, stream_id, frame_count);
        last_rate_check = now;
        frame_count = 0;
    }
    
    if (is_keyframe && frame_count % 500 == 0) {  // Log keyframes more frequently for debugging
        log_info("WebRTC: Keyframe processed for stream %u", stream_id);
    }
    
    // Find camera_id for this stream_id from stream configuration
    uint32_t camera_id = 0;
    {
        boost::shared_lock<boost::shared_mutex> streams_lock(streams_mutex_);
        for (const auto& [stream_key, stream_info] : streams_) {
            if (stream_info->stream_id == stream_id) {
                camera_id = stream_info->camera_id;
                break;
            }
        }
    }
    
    // If no stream found, log error and skip frame
    if (camera_id == 0) {
        log_error("WebRTC: Received frame for unknown stream_id %u, skipping", stream_id);
        return;
    }
    
    auto frame = std::make_shared<FrameData>(
        frame_data, data_size, frame_hdr->pts_usec, is_keyframe, camera_id, stream_id
    );
    
    {
        std::lock_guard<std::mutex> lock(frame_queue_mutex_);
        // Increase frame queue size for 25fps video (25fps * 10 seconds = 250 frames)
        if (frame_queue_.size() > 250) { 
            frame_queue_.pop();
            // Log frame drops to identify bottlenecks
            static uint64_t dropped_frames = 0;
            if (++dropped_frames % 100 == 0) {
                log_info("WebRTC: Dropped %llu frames due to queue overflow", dropped_frames);
            }
        }
        frame_queue_.push(frame);
    }
    
    frame_queue_cv_.notify_one();
    total_frames_processed_++;
    total_bytes_processed_ += data_size;

}

std::string WebRTCService::createOffer(uint32_t camera_id, const std::string& viewer_id) {
    try {
        auto session = createViewerSession(camera_id, viewer_id);
        if (!session) return "";
        
        // Set local description to generate offer
        session->peer_connection->setLocalDescription();
        auto local_desc = session->peer_connection->localDescription();
        
        total_connections_created_++;
        log_info("Created offer for viewer %s on camera %u", viewer_id.c_str(), camera_id);
        
        return local_desc ? std::string(*local_desc) : "";
        
    } catch (const std::exception& e) {
        log_error("Failed to create offer: %s", e.what());
        return "";
    }
}

bool WebRTCService::setAnswer(uint32_t camera_id, const std::string& viewer_id, const std::string& answer) {
    try {
        std::string viewer_key = std::to_string(camera_id) + ":" + viewer_id;
        
        boost::shared_lock<boost::shared_mutex> lock(viewers_mutex_);
        auto it = viewers_.find(viewer_key);
        if (it == viewers_.end()) return false;
        
        auto session = it->second;
        rtc::Description answer_desc(answer, "answer");
        session->peer_connection->setRemoteDescription(answer_desc);
        session->is_connected = true;
        session->last_activity = std::chrono::steady_clock::now();
        
        log_info("Set answer for viewer %s on camera %u", viewer_id.c_str(), camera_id);
        return true;
        
    } catch (const std::exception& e) {
        log_error("Failed to set answer: %s", e.what());
        return false;
    }
}

bool WebRTCService::addIceCandidate(uint32_t camera_id, const std::string& viewer_id, 
                                   const std::string& candidate, const std::string& sdp_mid) {
    try {
        std::string viewer_key = std::to_string(camera_id) + ":" + viewer_id;
        
        boost::shared_lock<boost::shared_mutex> lock(viewers_mutex_);
        auto it = viewers_.find(viewer_key);
        if (it == viewers_.end()) return false;
        
        auto session = it->second;
        rtc::Candidate ice_candidate(candidate, sdp_mid);
        session->peer_connection->addRemoteCandidate(ice_candidate);
        session->last_activity = std::chrono::steady_clock::now();
        
        return true;
        
    } catch (const std::exception& e) {
        log_error("Failed to add ICE candidate: %s", e.what());
        return false;
    }
}

bool WebRTCService::dropViewer(uint32_t camera_id, const std::string& viewer_id) {
    std::string viewer_key = std::to_string(camera_id) + ":" + viewer_id;
    
    boost::unique_lock<boost::shared_mutex> lock(viewers_mutex_);
    auto it = viewers_.find(viewer_key);
    if (it != viewers_.end()) {
        viewers_.erase(it);
        total_connections_dropped_++;
        log_info("Dropped viewer %s on camera %u", viewer_id.c_str(), camera_id);
        return true;
    }
    return false;
}

void WebRTCService::startControlServer(const std::string& bind_address, uint16_t port) {
    if (!io_context_) return;
    
    control_server_ = std::make_unique<ControlServer>(*io_context_, *this);
    control_server_->start(bind_address, port);
}

std::shared_ptr<ViewerSession> WebRTCService::createViewerSession(uint32_t camera_id, const std::string& viewer_id) {
    std::string viewer_key = std::to_string(camera_id) + ":" + viewer_id;
    
    boost::unique_lock<boost::shared_mutex> lock(viewers_mutex_);
    
    // Remove existing session if any
    viewers_.erase(viewer_key);
    
    // Create new session
    uint32_t ssrc = next_ssrc_++;
    auto session = std::make_shared<ViewerSession>(viewer_id, camera_id, ssrc);
    session->peer_connection = peer_manager_->createPeerConnection(viewer_id);
    
    peer_manager_->addVideoTrack(session->peer_connection, ssrc);
    
    viewers_[viewer_key] = session;
    return session;
}

void WebRTCService::processFrameQueue() {
    while (running_) {
        std::unique_lock<std::mutex> lock(frame_queue_mutex_);
        frame_queue_cv_.wait(lock, [this] { return !frame_queue_.empty() || !running_; });
        
        if (!running_) break;
        
        while (!frame_queue_.empty()) {
            auto frame = frame_queue_.front();
            frame_queue_.pop();
            lock.unlock();
            
            // Send frame to all viewers of this camera
            boost::shared_lock<boost::shared_mutex> viewers_lock(viewers_mutex_);
            int active_viewers = 0;
            auto now = std::chrono::steady_clock::now();
            for (const auto& [viewer_key, session] : viewers_) {
                if (session->camera_id == frame->camera_id && session->is_connected) {
                    peer_manager_->sendFrame(std::to_string(session->ssrc), *frame);
                    // Update last_activity to prevent stale cleanup of active viewers
                    session->last_activity = now;
                    active_viewers++;
                }
            }
            viewers_lock.unlock();
            
            if (active_viewers == 0) {
                // Reduce logging frequency - only log every 10 seconds worth of frames
                static auto last_no_viewers_log = std::chrono::steady_clock::now();
                auto now = std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::seconds>(now - last_no_viewers_log).count() >= 10) {
                    log_debug("WebRTC: No active viewers for camera %u, frames being discarded", frame->camera_id);
                    last_no_viewers_log = now;
                }
            } else {
                // Track outbound fps for performance monitoring
                static uint64_t frames_sent = 0;
                static auto last_outbound_fps_check = std::chrono::steady_clock::now();
                frames_sent += active_viewers;  // Count total frames sent to all viewers
                
                auto now_fps = std::chrono::steady_clock::now();
                auto elapsed_fps = std::chrono::duration_cast<std::chrono::seconds>(now_fps - last_outbound_fps_check);
                if (elapsed_fps.count() >= 5) {  // Check outbound fps every 5 seconds
                    double outbound_fps = static_cast<double>(frames_sent) / elapsed_fps.count() / active_viewers;
                    log_info("WebRTC: Sending %.1f fps to %d viewers for camera %u (total frames sent: %llu)", 
                             outbound_fps, active_viewers, frame->camera_id, frames_sent);
                    last_outbound_fps_check = now_fps;
                    frames_sent = 0;
                }
                
                // Reduce logging frequency - only log every 5 seconds worth of frames when viewers are active
                static auto last_viewers_log = std::chrono::steady_clock::now();
                auto now = std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::seconds>(now - last_viewers_log).count() >= 5) {
                    log_info("WebRTC: Frame sent to %d viewers for camera %u", active_viewers, frame->camera_id);
                    last_viewers_log = now;
                }
            }
            
            // Update stream statistics
            std::string stream_key = std::to_string(frame->camera_id) + ":" + std::to_string(frame->stream_id);
            boost::shared_lock<boost::shared_mutex> streams_lock(streams_mutex_);
            auto it = streams_.find(stream_key);
            if (it != streams_.end()) {
                it->second->frame_count++;
                it->second->bytes_sent += frame->data.size();
            }
            
            lock.lock();
        }
    }
}

void WebRTCService::cleanupStaleConnections() {
    auto now = std::chrono::steady_clock::now();
    auto timeout = std::chrono::minutes(5);
    
    boost::unique_lock<boost::shared_mutex> lock(viewers_mutex_);
    
    auto it = viewers_.begin();
    while (it != viewers_.end()) {
        auto& session = it->second;
        if (!session->is_connected || (now - session->last_activity) > timeout) {
            log_info("Cleaning up stale viewer %s", session->viewer_id.c_str());
            it = viewers_.erase(it);
            total_connections_dropped_++;
        } else {
            ++it;
        }
    }
}

json WebRTCService::getStatistics() const {
    json stats;
    
    stats["total_frames_processed"] = total_frames_processed_.load();
    stats["total_bytes_processed"] = total_bytes_processed_.load();
    stats["total_connections_created"] = total_connections_created_.load();
    stats["total_connections_dropped"] = total_connections_dropped_.load();
    
    // Stream statistics
    json streams_stats = json::array();
    {
        boost::shared_lock<boost::shared_mutex> lock(streams_mutex_);
        for (const auto& [stream_key, stream] : streams_) {
            json stream_stat;
            stream_stat["camera_id"] = stream->camera_id;
            stream_stat["stream_id"] = stream->stream_id;
            stream_stat["codec"] = stream->codec;
            stream_stat["width"] = stream->width;
            stream_stat["height"] = stream->height;
            stream_stat["frame_count"] = stream->frame_count.load();
            stream_stat["bytes_sent"] = stream->bytes_sent.load();
            streams_stats.push_back(stream_stat);
        }
    }
    stats["streams"] = streams_stats;
    
    // Active viewers
    json viewers_stats = json::array();
    {
        boost::shared_lock<boost::shared_mutex> lock(viewers_mutex_);
        for (const auto& [viewer_key, session] : viewers_) {
            json viewer_stat;
            viewer_stat["viewer_id"] = session->viewer_id;
            viewer_stat["camera_id"] = session->camera_id;
            viewer_stat["is_connected"] = session->is_connected.load();
            viewer_stat["ssrc"] = session->ssrc;
            viewers_stats.push_back(viewer_stat);
        }
    }
    stats["viewers"] = viewers_stats;
    
    return stats;
}

// =============================================================================
// PLUGIN INSTANCE
// =============================================================================

struct WebRTCPluginInstance {
    uint32_t camera_id;
    uint32_t stream_id;
    std::string codec;
    bool initialized = false;
    
    WebRTCPluginInstance() = default;
    WebRTCPluginInstance(uint32_t cam_id, uint32_t str_id, const std::string& codec_type)
        : camera_id(cam_id), stream_id(str_id), codec(codec_type), initialized(true) {}
    
    ~WebRTCPluginInstance() {
        if (initialized) {
            WebRTCService::getInstance().unregisterStream(camera_id, stream_id);
        }
    }
};

// =============================================================================
// PLUGIN LIFECYCLE FUNCTIONS
// =============================================================================

static void* webrtc_init(const char* config_json) {
    try {
        auto instance = std::make_unique<WebRTCPluginInstance>();
        
        if (config_json) {
            json config = json::parse(config_json);
            instance->camera_id = config.value("camera_id", 0);
            instance->stream_id = config.value("stream_id", 0);
            instance->codec = config.value("codec", "h264");
            
            // Start control server on first instance
            static std::once_flag server_started;
            std::call_once(server_started, []() {
                WebRTCService::getInstance().initialize();
                WebRTCService::getInstance().startControlServer("127.0.0.1", 9050);
            });
        }
        
        instance->initialized = true;
        log_info("WebRTC plugin initialized for camera %u, stream %u", 
               instance->camera_id, instance->stream_id);
        
        return instance.release();
        
    } catch (const std::exception& e) {
        log_error("Failed to initialize WebRTC plugin: %s", e.what());
        return nullptr;
    }
}

static void webrtc_shutdown(void* instance_ptr) {
    if (instance_ptr) {
        auto instance = static_cast<WebRTCPluginInstance*>(instance_ptr);
        delete instance;
        log_info("WebRTC plugin shutdown");
    }
}

static int webrtc_start(zm_plugin_t* plugin, zm_host_api_t* host, void* host_ctx, const char* json_cfg) {
    if (!plugin || !host || !json_cfg) return -1;
    
    // Set global host API for logging
    g_host_api = host;
    g_host_ctx = host_ctx;
    
    try {
        // Parse configuration
        json config = json::parse(json_cfg);
        uint32_t camera_id = config.value("camera_id", 0);
        uint32_t stream_id = config.value("stream_id", 0);
        std::string codec = config.value("codec", "h264");
        
        // Create plugin instance
        auto instance = new WebRTCPluginInstance(camera_id, stream_id, codec);
        plugin->instance = instance;
        
        // Initialize the WebRTC service singleton and start control server (once)
        static std::once_flag server_started;
        std::call_once(server_started, []() {
            WebRTCService::getInstance().initialize();
            WebRTCService::getInstance().startControlServer("127.0.0.1", 9050);
        });
        
        // Register stream
        WebRTCService::getInstance().registerStream(camera_id, stream_id, codec, nullptr);
        
        log_info("WebRTC plugin started for camera %u, stream %u", camera_id, stream_id);
        return 0;
        
    } catch (const std::exception& e) {
        log_error("Failed to start WebRTC plugin: %s", e.what());
        return -1;
    }
}

static void webrtc_stop(zm_plugin_t* plugin) {
    if (!plugin || !plugin->instance) return;
    
    auto instance = static_cast<WebRTCPluginInstance*>(plugin->instance);
    WebRTCService::getInstance().unregisterStream(instance->camera_id, instance->stream_id);
    
    log_info("WebRTC plugin stopped for camera %u, stream %u", 
           instance->camera_id, instance->stream_id);
    
    delete instance;
    plugin->instance = nullptr;
}

static void webrtc_on_frame(zm_plugin_t* plugin, const void* buf, size_t size) {
    if (!plugin || !plugin->instance || !buf || size < sizeof(zm_frame_hdr_t)) return;
    
    auto instance = static_cast<WebRTCPluginInstance*>(plugin->instance);
    const zm_frame_hdr_t* frame_hdr = static_cast<const zm_frame_hdr_t*>(buf);
    // Video-only output; ignore audio frames now that the pipeline carries audio.
    if (frame_hdr->hw_type == ZM_FRAME_COMPRESSED_AUDIO) return;
    const uint8_t* frame_data = static_cast<const uint8_t*>(buf) + sizeof(zm_frame_hdr_t);
    size_t frame_size = size - sizeof(zm_frame_hdr_t);

    WebRTCService::getInstance().pushFrame(frame_hdr, frame_data, frame_size);
}

// =============================================================================
// PLUGIN EXPORT
// =============================================================================

extern "C" {
    __attribute__((visibility("default"))) void zm_plugin_init(zm_plugin_t* plugin) {
        if (!plugin) return;
        plugin->version = 1;
        plugin->type = ZM_PLUGIN_OUTPUT;
        plugin->start = webrtc_start;
        plugin->stop = webrtc_stop;
        plugin->on_frame = webrtc_on_frame;
        plugin->instance = nullptr;
    }
}
