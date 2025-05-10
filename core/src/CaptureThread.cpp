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
static void frame_to_ring_callback(const zm_frame_hdr_t* hdr, const void* payload, size_t payload_size) {
    // Get the ShmRing pointer from the context passed via hw_type (hack, but works for now)
    ShmRing* ring = static_cast<ShmRing*>(reinterpret_cast<void*>(static_cast<uintptr_t>(hdr->hw_type)));
    if (!ring) {
        std::cerr << "CaptureThread: Invalid ring buffer in callback" << std::endl;
        return;
    }
    
    if (!payload || payload_size == 0) {
        std::cerr << "CaptureThread: Empty frame payload" << std::endl;
        return;
    }
    
    // Serialize the header and payload together
    const size_t headerSize = sizeof(zm_frame_hdr_t);
    std::vector<char> buffer(headerSize + payload_size);
    
    // Copy header to buffer
    std::memcpy(buffer.data(), hdr, headerSize);
    
    // Copy frame data
    std::memcpy(buffer.data() + headerSize, payload, payload_size);
    
    // Push combined data to ring
    ring->push(buffer.data(), buffer.size());
}

CaptureThread::CaptureThread(zm_plugin_t* inputPlugin,
                             ShmRing& ring,
                             const std::vector<zm_plugin_t*>& outputs)
    : inputPlugin_(inputPlugin)
    , ring_(ring)
    , outputs_(outputs) {
    // Try to register the frame callback with the plugin
    try {
        register_frame_callback(inputPlugin_, frame_to_ring_callback, &ring_);
        std::cout << "CaptureThread: Successfully registered frame callback with plugin" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "CaptureThread: Failed to register frame callback: " << e.what() << std::endl;
    }
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
    // Start the plugin
    // The new plugin API expects: (plugin, host_api, host_ctx, json_cfg)
    // For now, pass nullptrs for host_api, host_ctx, and json_cfg if not available
    if (inputPlugin_->start)
        inputPlugin_->start(inputPlugin_, nullptr, nullptr, nullptr);
    
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
            
            // Extract header from buffer
            zm_frame_hdr_t* hdr = reinterpret_cast<zm_frame_hdr_t*>(buffer.data());
            const void* payload = buffer.data() + headerSize;
            // size_t payload_size = size - headerSize; // unused
            
            // Fan out to all output plugins
            for (auto out : outputs_) {
                if (out && out->on_frame) {
                    // The new API expects (plugin, hdr, payload)
                    out->on_frame(out, hdr, payload);
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
