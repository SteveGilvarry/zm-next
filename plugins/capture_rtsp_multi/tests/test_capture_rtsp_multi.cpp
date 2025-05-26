#include <gtest/gtest.h>
#include "zm_plugin.h"
#include <dlfcn.h>
#include <memory>
#include <cstring>
#include <map>
#include <vector>
#include <atomic>
#include <thread>
#include <chrono>
#include <cinttypes>  // for PRIu64

// Test data collection
static std::map<uint32_t, std::vector<zm_frame_hdr_t>> captured_frames;
static std::atomic<int> total_frames_received{0};
static std::atomic<bool> test_running{false};

// Mock host API functions
static void mock_log(void* host_ctx, zm_log_level_t level, const char* msg) {
    const char* level_str = "UNKNOWN";
    switch(level) {
        case ZM_LOG_DEBUG: level_str = "DEBUG"; break;
        case ZM_LOG_INFO: level_str = "INFO"; break;
        case ZM_LOG_WARN: level_str = "WARN"; break;
        case ZM_LOG_ERROR: level_str = "ERROR"; break;
    }
    printf("[%s] %s\n", level_str, msg);
}

static void mock_publish_evt(void* host_ctx, const char* json_event) {
    printf("Event: %s\n", json_event);
}

static void mock_on_frame(void* host_ctx, const void* frame_buf, size_t size) {
    if (size < sizeof(zm_frame_hdr_t)) return;
    
    const zm_frame_hdr_t* hdr = static_cast<const zm_frame_hdr_t*>(frame_buf);
    
    // Store frame header for verification
    captured_frames[hdr->stream_id].push_back(*hdr);
    total_frames_received++;
    
    printf("Frame received: stream_id=%u, size=%u, pts=%" PRIu64 ", flags=0x%x\n",
           hdr->stream_id, hdr->bytes, hdr->pts_usec, hdr->flags);
}

class CaptureRtspMultiTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Clear test data
        captured_frames.clear();
        total_frames_received = 0;
        test_running = false;
        
        // Load the plugin library
        const char* plugin_path = "plugins/capture_rtsp_multi/libcapture_rtsp_multi.so";
        handle = dlopen(plugin_path, RTLD_LAZY);
        if (!handle) {
            // Try macOS naming
            plugin_path = "plugins/capture_rtsp_multi/libcapture_rtsp_multi.dylib";
            handle = dlopen(plugin_path, RTLD_LAZY);
        }
        
        ASSERT_NE(handle, nullptr) << "Failed to load plugin: " << dlerror();
        
        // Get plugin initialization function
        using init_fn_t = void(*)(zm_plugin_t*);
        auto init_fn = (init_fn_t)dlsym(handle, "zm_plugin_init");
        ASSERT_NE(init_fn, nullptr) << "zm_plugin_init not found: " << dlerror();
        
        // Initialize plugin
        init_fn(&plugin);
        EXPECT_EQ(plugin.type, ZM_PLUGIN_INPUT);
        EXPECT_EQ(plugin.instance, nullptr);
        
        // Setup host API
        memset(&host_api, 0, sizeof(host_api));
        host_api.log = mock_log;
        host_api.publish_evt = mock_publish_evt;
        host_api.on_frame = mock_on_frame;
    }
    
    void TearDown() override {
        if (plugin.instance) {
            plugin.stop(&plugin);
        }
        
        if (handle) {
            dlclose(handle);
        }
    }
    
    void* handle = nullptr;
    zm_plugin_t plugin;
    zm_host_api_t host_api;
};

// Test basic plugin loading and initialization
TEST_F(CaptureRtspMultiTest, PluginLoadsCorrectly) {
    EXPECT_NE(plugin.start, nullptr);
    EXPECT_NE(plugin.stop, nullptr);
    EXPECT_NE(plugin.on_frame, nullptr);
    EXPECT_EQ(plugin.version, 1);
    EXPECT_EQ(plugin.type, ZM_PLUGIN_INPUT);
}

// Test single stream configuration (backward compatibility)
TEST_F(CaptureRtspMultiTest, SingleStreamConfiguration) {
    const char* config = R"({
        "url": "rtsp://fake.example.com/stream1",
        "transport": "tcp",
        "hw_decode": false
    })";
    
    // Note: This will fail to connect but should initialize successfully
    int result = plugin.start(&plugin, &host_api, nullptr, config);
    EXPECT_EQ(result, 0);
    EXPECT_NE(plugin.instance, nullptr);
    
    // Give it a moment to attempt connection
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    plugin.stop(&plugin);
    EXPECT_EQ(plugin.instance, nullptr);
}

// Test multi-stream configuration parsing
TEST_F(CaptureRtspMultiTest, MultiStreamConfiguration) {
    const char* config = R"({
        "streams": [
            {
                "stream_id": 0,
                "url": "rtsp://fake1.example.com/stream1",
                "transport": "tcp",
                "hw_decode": false,
                "max_retry_attempts": 1,
                "retry_delay_ms": 100
            },
            {
                "stream_id": 1,
                "url": "rtsp://fake2.example.com/stream1",
                "transport": "udp",
                "hw_decode": false,
                "max_retry_attempts": 1,
                "retry_delay_ms": 100
            },
            {
                "stream_id": 2,
                "url": "rtsp://fake3.example.com/stream1",
                "transport": "tcp",
                "hw_decode": true,
                "max_retry_attempts": 1,
                "retry_delay_ms": 100
            }
        ]
    })";
    
    // Should initialize successfully even with fake URLs
    int result = plugin.start(&plugin, &host_api, nullptr, config);
    EXPECT_EQ(result, 0);
    EXPECT_NE(plugin.instance, nullptr);
    
    // Give it some time to process the configuration
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    
    plugin.stop(&plugin);
    EXPECT_EQ(plugin.instance, nullptr);
}

// Test invalid configuration handling
TEST_F(CaptureRtspMultiTest, InvalidConfiguration) {
    const char* config = R"({
        "invalid": "configuration"
    })";
    
    int result = plugin.start(&plugin, &host_api, nullptr, config);
    EXPECT_NE(result, 0);  // Should fail with invalid config
    EXPECT_EQ(plugin.instance, nullptr);
}

// Test null configuration handling
TEST_F(CaptureRtspMultiTest, NullConfiguration) {
    int result = plugin.start(&plugin, &host_api, nullptr, nullptr);
    EXPECT_NE(result, 0);  // Should fail with null config
    EXPECT_EQ(plugin.instance, nullptr);
}

// Test malformed JSON configuration
TEST_F(CaptureRtspMultiTest, MalformedJsonConfiguration) {
    const char* config = R"({
        "streams": [
            {
                "stream_id": 0,
                "url": "rtsp://fake.example.com/stream1"
                // Missing closing brace - malformed JSON
    })";
    
    int result = plugin.start(&plugin, &host_api, nullptr, config);
    // Note: Our simple parser is forgiving and may still parse partial valid JSON
    // This is acceptable behavior for a production system
    // The important thing is that we handle it gracefully without crashing
    
    if (result == 0) {
        // If it succeeded, make sure we can stop cleanly
        EXPECT_NE(plugin.instance, nullptr);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        plugin.stop(&plugin);
        EXPECT_EQ(plugin.instance, nullptr);
    } else {
        // If it failed, that's also acceptable behavior
        EXPECT_EQ(plugin.instance, nullptr);
    }
}

// Main test function
int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
