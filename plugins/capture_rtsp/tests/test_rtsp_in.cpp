// RTSP input plugin test

#include <gtest/gtest.h>
#include "zm_plugin_v1.h"
#include <cstring>
#include <thread>
#include <chrono>
#include <cinttypes> // For PRIu64

// Mock host API
static zm_frame_hdr_t last_frame_hdr;
static bool frame_received = false;
static bool key_frame_received = false;
static int frame_count = 0;

static void mock_log(void* host_ctx, zm_log_level_t level, const char* msg) {
    printf("[%d] %s\n", level, msg);
}

static void mock_publish_evt(void* host_ctx, const char* json_event) {
    printf("Event: %s\n", json_event);
}

static void mock_on_frame(void* host_ctx, const void* frame_data, size_t frame_size) {
    // Frame data starts with header
    if (frame_size >= sizeof(zm_frame_hdr_t)) {
        memcpy(&last_frame_hdr, frame_data, sizeof(zm_frame_hdr_t));
        frame_received = true;
        frame_count++;
        
        // Check if it's a keyframe
        if (last_frame_hdr.flags & 1) {
            key_frame_received = true;
        }
    }
}

// Extern for the plugin init function
extern "C" void zm_plugin_init(zm_plugin_t* plugin);

class RtspPluginTest : public ::testing::Test {
protected:
    zm_plugin_t plugin;
    zm_host_api_t host;
    
    void SetUp() override {
        // Reset test state
        frame_received = false;
        key_frame_received = false;
        frame_count = 0;
        memset(&last_frame_hdr, 0, sizeof(last_frame_hdr));
        
        // Initialize plugin
        memset(&plugin, 0, sizeof(plugin));
        zm_plugin_init(&plugin);
        
        // Setup mock host API
        memset(&host, 0, sizeof(host));
        host.log = mock_log;
        host.publish_evt = mock_publish_evt;
        host.on_frame = mock_on_frame;
    }
    
    void TearDown() override {
        // Stop plugin if running
        if (plugin.stop) {
            plugin.stop(&plugin);
        }
        
        // Free plugin instance
        if (plugin.instance) {
            delete plugin.instance;
            plugin.instance = nullptr;
        }
    }
};

// Check if URL is available for testing
bool is_rtsp_url_available(const char* url) {
    // This is a placeholder. In a real test, you might:
    // 1. Try to establish a socket connection to the host:port
    // 2. Check if an environment variable is set
    // 3. Look for a running ffmpeg process serving RTSP
    
    // For now, just check if RTSP_TEST_URL env var is set
    if (getenv("RTSP_TEST_URL")) {
        return true;
    }
    
    // If not set, print a message
    printf("Skipping test: RTSP_TEST_URL environment variable not set\n");
    return false;
}

// Basic initialization test
TEST_F(RtspPluginTest, InitializesCorrectly) {
    ASSERT_NE(plugin.instance, nullptr);
    EXPECT_EQ(plugin.kind, ZM_PLUG_INPUT);
    EXPECT_EQ(plugin.version, 1);
    EXPECT_NE(plugin.start, nullptr);
    EXPECT_NE(plugin.stop, nullptr);
    EXPECT_EQ(plugin.on_frame, nullptr);  // INPUT plugins don't need this
}

// Test connection with invalid URL
TEST_F(RtspPluginTest, RejectsInvalidUrl) {
    const char* config = "{\"url\": \"rtsp://invalid.example.com:8554/nonexistent\"}";
    
    int result = plugin.start(&plugin, &host, this, config);
    
    // Should still return success (asynchronous connection attempts)
    EXPECT_EQ(result, 0);
    
    // Let it run for a short time to attempt connection
    std::this_thread::sleep_for(std::chrono::seconds(2));
    
    // Stop the plugin
    plugin.stop(&plugin);
    
    // Should not have received any frames
    EXPECT_EQ(frame_count, 0);
}

// Test with a real RTSP stream if available
TEST_F(RtspPluginTest, ConnectsToRealStream) {
    // Get URL from environment or use a default test URL
    const char* url = getenv("RTSP_TEST_URL");
    if (!url) {
        GTEST_SKIP() << "RTSP_TEST_URL environment variable not set";
        return;
    }
    
    char config[256];
    snprintf(config, sizeof(config), "{\"url\": \"%s\", \"hw_decode\": false}", url);
    
    int result = plugin.start(&plugin, &host, this, config);
    EXPECT_EQ(result, 0);
    
    // Wait for frames (up to 10 seconds)
    const int max_wait_iterations = 100;
    for (int i = 0; i < max_wait_iterations && !key_frame_received; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    // Stop the plugin
    plugin.stop(&plugin);
    
    // Should have received at least some frames and at least one keyframe
    EXPECT_TRUE(frame_received) << "No frames received";
    EXPECT_TRUE(key_frame_received) << "No keyframes received";
    EXPECT_GT(frame_count, 0) << "No frames counted";
    
    // Print frame info
    printf("Received %d frames, last frame: %ux%u, stream %u, pts %" PRIu64 "\n",
           frame_count, last_frame_hdr.width, last_frame_hdr.height,
           last_frame_hdr.stream_id, last_frame_hdr.pts_usec);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
