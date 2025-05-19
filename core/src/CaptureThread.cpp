#include "zm/CaptureThread.hpp"
#include <cstring>
#include <iostream>
#include <chrono>
#include <thread>

// Function prototype for the plugin's frame callback registration
extern "C" void register_frame_callback(zm_plugin_t* plugin, 
                                       void (*push_frame)(const zm_frame_hdr_t* hdr, const void* payload, size_t payload_size),
                                       void* callback_ctx);

// Stub implementation to satisfy linking. Plugins may register a frame callback.
extern "C" void register_frame_callback(zm_plugin_t* plugin,
                                       void (*push_frame)(const zm_frame_hdr_t* hdr, const void* payload, size_t payload_size),
                                       void* callback_ctx) {
    // Placeholder: actual binding occurs inside plugin init or host_api
}

namespace zm {

// Struct to combine frame header and buffer
struct FrameData {
    zm_frame_hdr_t hdr;
    std::vector<char> data;
};

// Static callback for frame data from plugin to ShmRing
// Adapter to match zm_host_api_t::on_frame signature
// Adapter for host_api.publish_evt to push JSON events into ring
static void host_api_publish_evt_adapter(void* host_ctx, const char* json_event) {
    if (!host_ctx || !json_event) return;
    ShmRing* ring = static_cast<ShmRing*>(host_ctx);
    size_t len = strlen(json_event);
    ring->push(json_event, len);
}
// Adapter to match zm_host_api_t::on_frame signature
static void host_api_on_frame_adapter(void* host_ctx, const void* frame_buf, size_t frame_size) {
    if (!host_ctx || !frame_buf || frame_size < sizeof(zm_frame_hdr_t)) return;
    ShmRing* ring = static_cast<ShmRing*>(host_ctx);
    const zm_frame_hdr_t* hdr = static_cast<const zm_frame_hdr_t*>(frame_buf);
    const void* payload = static_cast<const uint8_t*>(frame_buf) + sizeof(zm_frame_hdr_t);
    size_t payload_size = frame_size - sizeof(zm_frame_hdr_t);
    // Optionally, use zm_log here if available (but for now, suppress or replace with zm_log if needed)
    // Serialize the header and payload together
    const size_t headerSize = sizeof(zm_frame_hdr_t);
    std::vector<char> buffer(headerSize + payload_size);
    std::memcpy(buffer.data(), hdr, headerSize);
    std::memcpy(buffer.data() + headerSize, payload, payload_size);
    ring->push(buffer.data(), buffer.size());
}


CaptureThread::CaptureThread(zm_plugin_t* inputPlugin,
                             ShmRing& ring,
                             const std::vector<zm_plugin_t*>& outputs,
                             const std::string& inputConfig)
    : inputPlugin_(inputPlugin)
    , ring_(ring)
    , outputs_(outputs)
    , inputConfig_(inputConfig) {
    // No need to register legacy frame callback; host_api->on_frame is used for input plugins
}

CaptureThread::~CaptureThread() {
    stop();
}

void CaptureThread::start() {
    if (running_.exchange(true))
        return;
    thread_ = std::thread(&CaptureThread::run, this);
}

void CaptureThread::stop() {
    if (!running_.exchange(false))
        return;
    if (thread_.joinable())
        thread_.join();
}

void CaptureThread::run() {
    // Set up host API for input plugin, wiring on_frame to our ring buffer callback
    zm_host_api_t host_api = {};
    // Route plugin logs to stdout so VS Code Debug Console captures them
    host_api.log = [](void* /*ctx*/, zm_log_level_t level, const char* msg) {
        const char* lvl = "INFO";
        if (level == ZM_LOG_DEBUG) lvl = "DEBUG";
        else if (level == ZM_LOG_WARN) lvl = "WARN";
        else if (level == ZM_LOG_ERROR) lvl = "ERROR";
        std::cout << "[PLUGIN][" << lvl << "] " << (msg ? msg : "(null)") << std::endl;
    };
    host_api.publish_evt = host_api_publish_evt_adapter; // forward events into ring
    host_api.on_frame = host_api_on_frame_adapter;
    // Pass the ring buffer as host_ctx so the callback can access it
    void* host_ctx = &ring_;
    // Log host_api pointer and on_frame value for debugging
    std::cerr << "CaptureThread::run: host_api=" << (void*)&host_api
              << ", on_frame=" << (void*)host_api.on_frame
              << ", log=" << (void*)host_api.log
              << ", publish_evt=" << (void*)host_api.publish_evt << std::endl;
    if (inputPlugin_->start)
        inputPlugin_->start(inputPlugin_, &host_api, host_ctx, inputConfig_.c_str());
    
    // Process frames from ring buffer
    const size_t headerSize = sizeof(zm_frame_hdr_t);
    const size_t MAX_BUFFER = 4 * 1024 * 1024; // 4MB buffer for frame data
    std::vector<char> buffer(MAX_BUFFER);
    size_t size = 0;
    
    // Main processing loop
    while (running_) {
        // Pop combined frame data from the ring buffer
        if (ring_.pop(buffer.data(), size)) {
            if (size <= headerSize) {
                std::cerr << "CaptureThread: Received invalid data size" << std::endl;
                continue;
            }
            
            // Fan out to all output plugins using the new single-buffer API
            for (auto out : outputs_) {
                if (out && out->on_frame) {
                    // The new API expects (plugin, buf, size)
                    out->on_frame(out, buffer.data(), size);
                }
            }
        } else {
            // Sleep a bit if no frames to avoid busy loop
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    
    // Stop the plugin when we're done
    inputPlugin_->stop(inputPlugin_);
}

} // namespace zm
