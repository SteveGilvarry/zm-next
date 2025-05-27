#include <zm_plugin.h>
#include <nlohmann/json.hpp>
#include <rtc/rtc.hpp>
#include <rtc/rtppacketizationconfig.hpp>
#include <rtc/h264rtppacketizer.hpp>
#include <rtc/rtcpsrreporter.hpp>
#include <rtc/nalunit.hpp>
#include <sstream>
#include <iomanip>
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <queue>
#include <unordered_map>
#include <chrono>
#include <atomic>
#include <fstream>
#include <filesystem>

#ifdef __cplusplus
extern "C" {
#endif
#include <libavformat/avformat.h>
#include <libavutil/time.h>
#include <libavutil/base64.h>
#include <libavcodec/avcodec.h>
#ifdef __cplusplus
}
#endif

using json = nlohmann::json;

struct WebRTCClient {
    std::string id;
    std::shared_ptr<rtc::PeerConnection> peer_connection;
    std::shared_ptr<rtc::Track> video_track;
    std::shared_ptr<rtc::RtcpSrReporter> sr_reporter;
    std::shared_ptr<rtc::H264RtpPacketizer> h264_packetizer;
    std::chrono::steady_clock::time_point last_activity;
    bool is_connected = false;
    uint32_t ssrc;
    uint16_t sequence_number = 0;
    uint32_t timestamp_offset = 0;
};

struct WebRTCInstance {
    // Configuration
    std::string bind_address = "0.0.0.0";
    int port = 8080;
    std::string ice_servers;
    std::vector<uint32_t> stream_filter;  // If empty, accept all streams
    int max_clients = 10;
    int client_timeout_seconds = 30;
    bool enable_simulcast = false;
    
    // Bridge communication
    std::string bridge_event_dir = "signaling/plugin-events";
    std::string bridge_response_dir = "signaling/plugin-responses";
    std::thread bridge_thread;
    std::atomic<bool> bridge_running{false};
    
    // WebRTC components
    std::shared_ptr<rtc::Configuration> rtc_config;
    
    // Client management
    std::unordered_map<std::string, std::unique_ptr<WebRTCClient>> clients;
    std::mutex clients_mutex;
    
    // Frame processing
    std::queue<std::vector<uint8_t>> frame_queue;
    std::mutex frame_mutex;
    std::condition_variable frame_cv;
    std::thread processing_thread;
    bool should_stop = false;
    
    // Stream metadata
    AVCodecParameters* metadata_codecpar = nullptr;
    bool has_metadata = false;
    std::mutex metadata_mutex;
    
    // Host API
    zm_host_api_t* host = nullptr;
    void* host_ctx = nullptr;
    
    // Statistics
    uint64_t frames_sent = 0;
    uint64_t bytes_sent = 0;
    uint64_t clients_connected = 0;
    uint64_t clients_disconnected = 0;
};

static void log(WebRTCInstance* inst, zm_log_level_t level, const char* fmt, ...) {
    if (!inst || !inst->host || !inst->host->log) return;
    va_list ap;
    va_start(ap, fmt);
    char buf[512];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    inst->host->log(inst->host_ctx, level, buf);
}

static void cleanup_disconnected_clients(WebRTCInstance* inst) {
    std::lock_guard<std::mutex> lock(inst->clients_mutex);
    auto now = std::chrono::steady_clock::now();
    
    auto it = inst->clients.begin();
    while (it != inst->clients.end()) {
        auto& client = it->second;
        auto duration = std::chrono::duration_cast<std::chrono::seconds>(
            now - client->last_activity).count();
            
        if (!client->is_connected || duration > inst->client_timeout_seconds) {
            log(inst, ZM_LOG_INFO, "Removing client %s (timeout or disconnected)", 
                client->id.c_str());
            inst->clients_disconnected++;
            it = inst->clients.erase(it);
        } else {
            ++it;
        }
    }
}

static std::string generate_client_id() {
    static std::atomic<uint32_t> counter{0};
    auto now = std::chrono::system_clock::now();
    auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
    std::ostringstream oss;
    oss << "client_" << timestamp << "_" << counter.fetch_add(1);
    return oss.str();
}

static void setup_ice_servers(WebRTCInstance* inst) {
    inst->rtc_config = std::make_shared<rtc::Configuration>();
    
    if (!inst->ice_servers.empty()) {
        try {
            auto ice_config = json::parse(inst->ice_servers);
            if (ice_config.is_array()) {
                for (const auto& server : ice_config) {
                    if (server.contains("urls")) {
                        std::string url = server["urls"];
                        inst->rtc_config->iceServers.emplace_back(url);
                        log(inst, ZM_LOG_INFO, "Added ICE server: %s", url.c_str());
                    }
                }
            }
        } catch (const std::exception& e) {
            log(inst, ZM_LOG_WARN, "Failed to parse ICE servers config: %s", e.what());
        }
    }
    
    // Add default STUN servers if none configured
    if (inst->rtc_config->iceServers.empty()) {
        inst->rtc_config->iceServers.emplace_back("stun:stun.l.google.com:19302");
        inst->rtc_config->iceServers.emplace_back("stun:stun1.l.google.com:19302");
        log(inst, ZM_LOG_INFO, "Using default STUN servers");
    }
}

static std::shared_ptr<WebRTCClient> create_webrtc_client(WebRTCInstance* inst, const std::string& client_id) {
    auto client = std::make_unique<WebRTCClient>();
    client->id = client_id;
    client->last_activity = std::chrono::steady_clock::now();
    client->ssrc = std::hash<std::string>{}(client_id) & 0x7FFFFFFF; // Ensure positive
    
    try {
        client->peer_connection = std::make_shared<rtc::PeerConnection>(*inst->rtc_config);
        
        // Set up callbacks
        client->peer_connection->onStateChange([inst, client_id](rtc::PeerConnection::State state) {
            std::lock_guard<std::mutex> lock(inst->clients_mutex);
            auto it = inst->clients.find(client_id);
            if (it != inst->clients.end()) {
                auto& client = it->second;
                switch (state) {
                    case rtc::PeerConnection::State::Connected:
                        client->is_connected = true;
                        inst->clients_connected++;
                        log(inst, ZM_LOG_INFO, "Client %s connected", client_id.c_str());
                        break;
                    case rtc::PeerConnection::State::Disconnected:
                    case rtc::PeerConnection::State::Failed:
                    case rtc::PeerConnection::State::Closed:
                        client->is_connected = false;
                        log(inst, ZM_LOG_INFO, "Client %s disconnected", client_id.c_str());
                        break;
                    default:
                        break;
                }
                
                // Send connection state to bridge
                std::string state_str;
                switch (state) {
                    case rtc::PeerConnection::State::New: state_str = "new"; break;
                    case rtc::PeerConnection::State::Connecting: state_str = "connecting"; break;
                    case rtc::PeerConnection::State::Connected: state_str = "connected"; break;
                    case rtc::PeerConnection::State::Disconnected: state_str = "disconnected"; break;
                    case rtc::PeerConnection::State::Failed: state_str = "failed"; break;
                    case rtc::PeerConnection::State::Closed: state_str = "closed"; break;
                }
                
                json state_response = {
                    {"type", "webrtc_connection_state"},
                    {"client_id", client_id},
                    {"state", state_str}
                };
                write_bridge_response(inst, state_response);
            }
        });
        
        client->peer_connection->onGatheringStateChange([inst, client_id](rtc::PeerConnection::GatheringState state) {
            if (state == rtc::PeerConnection::GatheringState::Complete) {
                log(inst, ZM_LOG_DEBUG, "ICE gathering complete for client %s", client_id.c_str());
            }
        });
        
        client->peer_connection->onLocalCandidate([inst, client_id](rtc::Candidate candidate) {
            log(inst, ZM_LOG_DEBUG, "Generated ICE candidate for client %s", client_id.c_str());
            
            json candidate_response = {
                {"type", "webrtc_ice_candidate"},
                {"client_id", client_id},
                {"candidate", {
                    {"candidate", candidate.candidate()},
                    {"sdpMid", candidate.mid()}
                }}
            };
            write_bridge_response(inst, candidate_response);
        });
        });
        
        // Create video track with H.264 description
        auto video_desc = rtc::Description::Video("video", rtc::Description::Direction::SendOnly);
        video_desc.addH264Codec(96, "profile-level-id=42e01f;level-asymmetry-allowed=1");
        
        auto video_track = client->peer_connection->addTrack(video_desc);
        client->video_track = video_track;
        
        // Create RTP packetization config
        auto rtp_config = std::make_shared<rtc::RtpPacketizationConfig>(
            client->ssrc, "video", 96, 90000); // H.264 standard clock rate
        
        // Create RTCP SR reporter
        client->sr_reporter = std::make_shared<rtc::RtcpSrReporter>(rtp_config);
        
        // Create H264 RTP packetizer
        client->h264_packetizer = std::make_shared<rtc::H264RtpPacketizer>(
            rtc::NalUnit::Separator::Length, rtp_config);
        
        // Chain the media handlers: H264 Packetizer -> RTCP SR Reporter
        video_track->setMediaHandler(client->h264_packetizer);
        client->h264_packetizer->addToChain(client->sr_reporter);
        
        log(inst, ZM_LOG_INFO, "Created WebRTC client %s with SSRC %u", client_id.c_str(), client->ssrc);
        
        return std::shared_ptr<WebRTCClient>(client.release());
        
    } catch (const std::exception& e) {
        log(inst, ZM_LOG_ERROR, "Failed to create WebRTC client %s: %s", client_id.c_str(), e.what());
        return nullptr;
    }
}

// Bridge communication functions
static void write_bridge_response(WebRTCInstance* inst, const json& response) {
    try {
        // Ensure response directory exists
        std::filesystem::create_directories(inst->bridge_response_dir);
        
        // Generate unique filename
        auto now = std::chrono::system_clock::now();
        auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        
        std::string filename = "response-" + std::to_string(timestamp) + "-" + 
                              std::to_string(rand() % 10000) + ".json";
        std::string filepath = inst->bridge_response_dir + "/" + filename;
        
        // Write response
        std::ofstream file(filepath);
        if (file.is_open()) {
            file << response.dump(2);
            file.close();
            log(inst, ZM_LOG_DEBUG, "Wrote bridge response: %s", filename.c_str());
        } else {
            log(inst, ZM_LOG_ERROR, "Failed to write bridge response: %s", filepath.c_str());
        }
    } catch (const std::exception& e) {
        log(inst, ZM_LOG_ERROR, "Error writing bridge response: %s", e.what());
    }
}

static void handle_bridge_peer_request(WebRTCInstance* inst, const json& request) {
    try {
        std::string client_id = request["client_id"];
        uint32_t stream_id = request.value("stream_id", 0);
        std::string offer_sdp = request["offer_sdp"];
        
        log(inst, ZM_LOG_INFO, "Bridge: Creating peer for client %s, stream %u", 
            client_id.c_str(), stream_id);
        
        // Create WebRTC client
        auto client = create_webrtc_client(inst, client_id);
        if (!client) {
            json error_response = {
                {"type", "webrtc_error"},
                {"client_id", client_id},
                {"error", "Failed to create WebRTC client"}
            };
            write_bridge_response(inst, error_response);
            return;
        }
        
        // Store client
        {
            std::lock_guard<std::mutex> lock(inst->clients_mutex);
            inst->clients[client_id] = std::move(client);
        }
        
        // Set remote description (offer)
        auto pc = inst->clients[client_id]->peer_connection;
        pc->setRemoteDescription(rtc::Description(offer_sdp, "offer"));
        
        // Create and set local description (answer)
        auto local_desc = pc->localDescription();
        if (local_desc) {
            json answer_response = {
                {"type", "webrtc_answer"},
                {"client_id", client_id},
                {"answer_sdp", std::string(*local_desc)}
            };
            write_bridge_response(inst, answer_response);
            log(inst, ZM_LOG_INFO, "Bridge: Generated answer for client %s", client_id.c_str());
        } else {
            json error_response = {
                {"type", "webrtc_error"},
                {"client_id", client_id},
                {"error", "Failed to generate answer"}
            };
            write_bridge_response(inst, error_response);
        }
        
    } catch (const std::exception& e) {
        log(inst, ZM_LOG_ERROR, "Error handling bridge peer request: %s", e.what());
        
        json error_response = {
            {"type", "webrtc_error"},
            {"client_id", request.value("client_id", "unknown")},
            {"error", e.what()}
        };
        write_bridge_response(inst, error_response);
    }
}

static void handle_bridge_ice_candidate(WebRTCInstance* inst, const json& request) {
    try {
        std::string client_id = request["client_id"];
        auto candidate_data = request["candidate"];
        
        log(inst, ZM_LOG_DEBUG, "Bridge: Received ICE candidate for client %s", client_id.c_str());
        
        std::lock_guard<std::mutex> lock(inst->clients_mutex);
        auto it = inst->clients.find(client_id);
        if (it != inst->clients.end() && it->second->peer_connection) {
            // Add remote ICE candidate
            rtc::Candidate candidate(candidate_data["candidate"], candidate_data["sdpMid"]);
            it->second->peer_connection->addRemoteCandidate(candidate);
            log(inst, ZM_LOG_DEBUG, "Added ICE candidate for client %s", client_id.c_str());
        } else {
            log(inst, ZM_LOG_WARN, "Bridge: Client %s not found for ICE candidate", client_id.c_str());
        }
        
    } catch (const std::exception& e) {
        log(inst, ZM_LOG_ERROR, "Error handling bridge ICE candidate: %s", e.what());
    }
}

static void handle_bridge_peer_remove(WebRTCInstance* inst, const json& request) {
    try {
        std::string client_id = request["client_id"];
        
        log(inst, ZM_LOG_INFO, "Bridge: Removing peer for client %s", client_id.c_str());
        
        std::lock_guard<std::mutex> lock(inst->clients_mutex);
        auto it = inst->clients.find(client_id);
        if (it != inst->clients.end()) {
            if (it->second->peer_connection) {
                it->second->peer_connection->close();
            }
            inst->clients.erase(it);
            inst->clients_disconnected++;
            log(inst, ZM_LOG_INFO, "Removed client %s", client_id.c_str());
        }
        
    } catch (const std::exception& e) {
        log(inst, ZM_LOG_ERROR, "Error handling bridge peer remove: %s", e.what());
    }
}

static void process_bridge_event_file(WebRTCInstance* inst, const std::string& filepath) {
    try {
        std::ifstream file(filepath);
        if (!file.is_open()) {
            log(inst, ZM_LOG_ERROR, "Failed to open bridge event file: %s", filepath.c_str());
            return;
        }
        
        json event;
        file >> event;
        file.close();
        
        std::string event_type = event.value("type", "unknown");
        log(inst, ZM_LOG_DEBUG, "Processing bridge event: %s", event_type.c_str());
        
        if (event_type == "webrtc_peer_request") {
            handle_bridge_peer_request(inst, event);
        } else if (event_type == "webrtc_ice_candidate") {
            handle_bridge_ice_candidate(inst, event);
        } else if (event_type == "webrtc_peer_remove") {
            handle_bridge_peer_remove(inst, event);
        } else {
            log(inst, ZM_LOG_WARN, "Unknown bridge event type: %s", event_type.c_str());
        }
        
        // Remove processed event file
        std::filesystem::remove(filepath);
        
    } catch (const std::exception& e) {
        log(inst, ZM_LOG_ERROR, "Error processing bridge event file: %s", e.what());
    }
}

static void bridge_communication_thread(WebRTCInstance* inst) {
    log(inst, ZM_LOG_INFO, "Bridge communication thread started");
    
    // Ensure event directory exists
    try {
        std::filesystem::create_directories(inst->bridge_event_dir);
        std::filesystem::create_directories(inst->bridge_response_dir);
    } catch (const std::exception& e) {
        log(inst, ZM_LOG_ERROR, "Failed to create bridge directories: %s", e.what());
        return;
    }
    
    while (inst->bridge_running) {
        try {
            // Check for new event files
            for (const auto& entry : std::filesystem::directory_iterator(inst->bridge_event_dir)) {
                if (entry.is_regular_file() && entry.path().extension() == ".json") {
                    process_bridge_event_file(inst, entry.path().string());
                }
            }
            
            // Sleep for a short time before checking again
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            
        } catch (const std::exception& e) {
            log(inst, ZM_LOG_ERROR, "Error in bridge communication thread: %s", e.what());
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
    
    log(inst, ZM_LOG_INFO, "Bridge communication thread stopped");
}

static void process_metadata_json(WebRTCInstance* inst, const char* buf, size_t size) {
    try {
        std::string js(buf, size);
        auto j = json::parse(js);
        if (j.value("event", "") == "StreamMetadata") {
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
            
            std::lock_guard<std::mutex> lock(inst->metadata_mutex);
            
            log(inst, ZM_LOG_INFO, "Processing WebRTC metadata for stream_id=%u", metadata_stream_id);
            
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
                size_t max_out_size = (ed_b64.length() * 3) / 4 + 1;
                uint8_t* out_buf = (uint8_t*)av_mallocz(max_out_size + AV_INPUT_BUFFER_PADDING_SIZE);
                
                if (out_buf) {
                    int decoded_size = av_base64_decode(out_buf, ed_b64.c_str(), max_out_size);
                    
                    if (decoded_size > 0) {
                        inst->metadata_codecpar->extradata = out_buf;
                        inst->metadata_codecpar->extradata_size = decoded_size;
                        log(inst, ZM_LOG_DEBUG, "WebRTC: decoded %d bytes of extradata", decoded_size);
                    } else {
                        av_free(out_buf);
                        inst->metadata_codecpar->extradata = nullptr;
                        inst->metadata_codecpar->extradata_size = 0;
                    }
                }
            }
            
            inst->has_metadata = true;
            log(inst, ZM_LOG_INFO, "WebRTC: received metadata, codec %d %dx%d",
                inst->metadata_codecpar->codec_id,
                inst->metadata_codecpar->width,
                inst->metadata_codecpar->height);
        }
    } catch (const std::exception& e) {
        log(inst, ZM_LOG_WARN, "Failed to parse JSON metadata in WebRTC: %s", e.what());
    }
}

static void send_frame_to_clients(WebRTCInstance* inst, const std::vector<uint8_t>& frame_data, 
                                 uint64_t timestamp, bool is_keyframe) {
    std::lock_guard<std::mutex> lock(inst->clients_mutex);
    
    if (inst->clients.empty()) {
        return;
    }
    
    auto now = std::chrono::steady_clock::now();
    
    for (auto& [client_id, client] : inst->clients) {
        if (!client->is_connected || !client->video_track) {
            continue;
        }
        
        try {
            // Update activity timestamp
            client->last_activity = now;
            
            // Create RTP packet - convert unsigned char to std::byte
            rtc::binary rtp_data;
            rtp_data.reserve(frame_data.size());
            for (unsigned char byte : frame_data) {
                rtp_data.push_back(static_cast<std::byte>(byte));
            }
            
            // Send via video track
            if (client->video_track->send(rtp_data)) {
                inst->frames_sent++;
                inst->bytes_sent += frame_data.size();
                
                if (is_keyframe) {
                    log(inst, ZM_LOG_DEBUG, "Sent keyframe to client %s (%zu bytes)", 
                        client_id.c_str(), frame_data.size());
                }
            } else {
                log(inst, ZM_LOG_WARN, "Failed to send frame to client %s", client_id.c_str());
            }
            
        } catch (const std::exception& e) {
            log(inst, ZM_LOG_ERROR, "Error sending frame to client %s: %s", 
                client_id.c_str(), e.what());
        }
    }
}

static void frame_processing_thread(WebRTCInstance* inst) {
    log(inst, ZM_LOG_INFO, "WebRTC frame processing thread started");
    
    while (!inst->should_stop) {
        std::unique_lock<std::mutex> lock(inst->frame_mutex);
        inst->frame_cv.wait(lock, [inst] { 
            return !inst->frame_queue.empty() || inst->should_stop; 
        });
        
        if (inst->should_stop) {
            break;
        }
        
        while (!inst->frame_queue.empty()) {
            auto frame_data = std::move(inst->frame_queue.front());
            inst->frame_queue.pop();
            lock.unlock();
            
            // Parse frame header
            if (frame_data.size() < sizeof(zm_frame_hdr_t)) {
                lock.lock();
                continue;
            }
            
            const zm_frame_hdr_t* hdr = reinterpret_cast<const zm_frame_hdr_t*>(frame_data.data());
            const uint8_t* payload = frame_data.data() + sizeof(zm_frame_hdr_t);
            
            bool is_keyframe = (hdr->flags & 1) != 0;
            
            // Send to all connected clients
            std::vector<uint8_t> payload_data(payload, payload + hdr->bytes);
            send_frame_to_clients(inst, payload_data, hdr->pts_usec, is_keyframe);
            
            lock.lock();
        }
        
        // Cleanup disconnected clients periodically
        static auto last_cleanup = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_cleanup).count() > 10) {
            lock.unlock();
            cleanup_disconnected_clients(inst);
            last_cleanup = now;
            lock.lock();
        }
    }
    
    log(inst, ZM_LOG_INFO, "WebRTC frame processing thread stopped");
}

static int handle_plugin_start(zm_plugin_t* plugin, zm_host_api_t* host, void* host_ctx, const char* json_cfg) {
    auto inst = new WebRTCInstance;
    inst->host = host;
    inst->host_ctx = host_ctx;
    
    try {
        auto j = json::parse(json_cfg);
        inst->bind_address = j.value("bind_address", "0.0.0.0");
        inst->port = j.value("port", 8080);
        inst->ice_servers = j.value("ice_servers", "");
        inst->max_clients = j.value("max_clients", 10);
        inst->client_timeout_seconds = j.value("client_timeout_seconds", 30);
        inst->enable_simulcast = j.value("enable_simulcast", false);
        
        // Parse stream filter if provided
        if (j.contains("stream_filter") && j["stream_filter"].is_array()) {
            for (const auto& sid : j["stream_filter"]) {
                if (sid.is_number_unsigned()) {
                    inst->stream_filter.push_back(sid.get<uint32_t>());
                }
            }
            log(inst, ZM_LOG_INFO, "WebRTC stream filter configured for %zu streams", 
                inst->stream_filter.size());
        }
        
    } catch (const std::exception& e) {
        log(inst, ZM_LOG_ERROR, "Invalid WebRTC config JSON: %s", e.what());
        delete inst;
        return -1;
    }
    
    // Initialize libdatachannel
    try {
        rtc::InitLogger(rtc::LogLevel::Warning);
        setup_ice_servers(inst);
        
        // Start frame processing thread
        inst->processing_thread = std::thread(frame_processing_thread, inst);
        
        // Start bridge communication thread
        inst->bridge_running = true;
        inst->bridge_thread = std::thread(bridge_communication_thread, inst);
        
        log(inst, ZM_LOG_INFO, "WebRTC output plugin started on %s:%d (max_clients=%d)", 
            inst->bind_address.c_str(), inst->port, inst->max_clients);
        log(inst, ZM_LOG_INFO, "Bridge communication enabled: events=%s, responses=%s",
            inst->bridge_event_dir.c_str(), inst->bridge_response_dir.c_str());
            
        // Publish status event
        json status_event = {
            {"event", "WebRTCStarted"},
            {"bind_address", inst->bind_address},
            {"port", inst->port},
            {"max_clients", inst->max_clients},
            {"bridge_enabled", true}
        };
        if (inst->host && inst->host->publish_evt) {
            inst->host->publish_evt(inst->host_ctx, status_event.dump().c_str());
        }
        
    } catch (const std::exception& e) {
        log(inst, ZM_LOG_ERROR, "Failed to initialize WebRTC: %s", e.what());
        delete inst;
        return -1;
    }
    
    plugin->instance = inst;
    return 0;
}

static void handle_plugin_stop(zm_plugin_t* plugin) {
    if (!plugin || !plugin->instance) return;
    
    auto inst = static_cast<WebRTCInstance*>(plugin->instance);
    
    log(inst, ZM_LOG_INFO, "Stopping WebRTC output plugin");
    
    // Signal threads to stop
    inst->should_stop = true;
    inst->bridge_running = false;
    inst->frame_cv.notify_all();
    
    // Wait for threads to finish
    if (inst->processing_thread.joinable()) {
        inst->processing_thread.join();
    }
    if (inst->bridge_thread.joinable()) {
        inst->bridge_thread.join();
    }
    
    // Cleanup clients
    {
        std::lock_guard<std::mutex> lock(inst->clients_mutex);
        for (auto& [client_id, client] : inst->clients) {
            if (client->peer_connection) {
                client->peer_connection->close();
            }
        }
        inst->clients.clear();
    }
    
    // Cleanup metadata
    if (inst->metadata_codecpar) {
        avcodec_parameters_free(&inst->metadata_codecpar);
    }
    
    // Publish final statistics
    json stats_event = {
        {"event", "WebRTCStats"},
        {"frames_sent", inst->frames_sent},
        {"bytes_sent", inst->bytes_sent},
        {"clients_connected", inst->clients_connected},
        {"clients_disconnected", inst->clients_disconnected}
    };
    if (inst->host && inst->host->publish_evt) {
        inst->host->publish_evt(inst->host_ctx, stats_event.dump().c_str());
    }
    
    log(inst, ZM_LOG_INFO, "WebRTC plugin stopped. Stats: frames=%lu, bytes=%lu, clients=%lu", 
        inst->frames_sent, inst->bytes_sent, inst->clients_connected);
    
    delete inst;
    plugin->instance = nullptr;
}

static void handle_on_frame(zm_plugin_t* plugin, const void* buf, size_t size) {
    if (!plugin || !plugin->instance || !buf || size < sizeof(zm_frame_hdr_t)) {
        return;
    }
    
    auto inst = static_cast<WebRTCInstance*>(plugin->instance);
    const zm_frame_hdr_t* hdr = static_cast<const zm_frame_hdr_t*>(buf);
    const char* payload = static_cast<const char*>(buf) + sizeof(zm_frame_hdr_t);
    
    // Check if this is JSON metadata
    if (hdr->bytes > 0 && payload[0] == '{') {
        process_metadata_json(inst, payload, hdr->bytes);
        return;
    }
    
    // Filter streams if configured
    if (!inst->stream_filter.empty()) {
        bool should_accept = std::find(inst->stream_filter.begin(), 
                                     inst->stream_filter.end(), 
                                     hdr->stream_id) != inst->stream_filter.end();
        if (!should_accept) {
            return;
        }
    }
    
    // Only process if we have connected clients
    {
        std::lock_guard<std::mutex> lock(inst->clients_mutex);
        if (inst->clients.empty()) {
            return;
        }
    }
    
    // Only process if we have metadata
    {
        std::lock_guard<std::mutex> lock(inst->metadata_mutex);
        if (!inst->has_metadata) {
            return;
        }
    }
    
    // Queue frame for processing
    {
        std::lock_guard<std::mutex> lock(inst->frame_mutex);
        
        // Limit queue size to prevent memory issues
        if (inst->frame_queue.size() > 100) {
            inst->frame_queue.pop(); // Remove oldest frame
        }
        
        // Copy frame data
        std::vector<uint8_t> frame_data(static_cast<const uint8_t*>(buf), 
                                       static_cast<const uint8_t*>(buf) + size);
        inst->frame_queue.push(std::move(frame_data));
    }
    
    inst->frame_cv.notify_one();
}

// Export plugin interface
extern "C" {
    void zm_plugin_init(zm_plugin_t* plugin) {
        plugin->version = 1;
        plugin->type = ZM_PLUGIN_OUTPUT;
        plugin->start = handle_plugin_start;
        plugin->stop = handle_plugin_stop;
        plugin->on_frame = handle_on_frame;
        plugin->instance = nullptr;
    }
}
