

// RTSP input plugin test
#include <gtest/gtest.h>
#include "zm_plugin.h"
#include <cstring>
#include <thread>
#include <chrono>
#include <cinttypes> // For PRIu64
#include <dlfcn.h>
#include <filesystem>
#include <cstdlib>
using namespace std::filesystem;

#ifdef _WIN32
#define PLUGIN_EXT ".dll"
#else
#ifdef __APPLE__
#define PLUGIN_EXT ".dylib"
#else
#define PLUGIN_EXT ".so"
#endif
#endif


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

// Single test: load plugin, connect to local ffmpeg RTSP test pattern, verify frames received
TEST(CaptureRtspPlugin, ReceivesFramesFromLocalFfmpegPattern) {
    std::string binDir = TEST_CMAKE_BINARY_DIR;
    path buildPath = path(binDir) / "plugins" / "capture_rtsp" / (std::string("capture_rtsp") + PLUGIN_EXT);
    path srcPath = path("plugins") / "capture_rtsp" / (std::string("capture_rtsp") + PLUGIN_EXT);
    path pluginPath;
    if (exists(buildPath)) {
        pluginPath = buildPath;
    } else if (exists(srcPath)) {
        pluginPath = srcPath;
    } else {
        FAIL() << "Cannot find plugin at " << buildPath << " or " << srcPath;
    }

    void* handle = dlopen(pluginPath.c_str(), RTLD_NOW);
    ASSERT_NE(handle, nullptr) << dlerror();

    using init_fn_t = void(*)(zm_plugin_t*);
    auto init_fn = (init_fn_t)dlsym(handle, "zm_plugin_init");
    ASSERT_NE(init_fn, nullptr) << dlerror();

    zm_plugin_t plugin;
    init_fn(&plugin);
    EXPECT_EQ(plugin.type, ZM_PLUGIN_INPUT);
    EXPECT_EQ(plugin.instance, nullptr);

    // Reset frame counters
    frame_received = false;
    key_frame_received = false;
    frame_count = 0;
    memset(&last_frame_hdr, 0, sizeof(last_frame_hdr));

    zm_host_api_t host = {0};
    host.log = mock_log;
    host.publish_evt = mock_publish_evt;
    host.on_frame = mock_on_frame;

    // Use local mediamtx RTSP test stream
    std::string config = "{\"url\":\"rtsp://localhost:8554/mystream\"}";
    int result = plugin.start(&plugin, &host, nullptr, config.c_str());
    ASSERT_EQ(result, 0);
    ASSERT_NE(plugin.instance, nullptr);

    // Wait for frames (up to 5 seconds)
    const int max_wait_iterations = 50;
    for (int i = 0; i < max_wait_iterations && !key_frame_received; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    plugin.stop(&plugin);
    EXPECT_EQ(plugin.instance, nullptr);

    // Should have received at least some frames and at least one keyframe
    EXPECT_TRUE(frame_received) << "No frames received";
    EXPECT_TRUE(key_frame_received) << "No keyframes received";
    EXPECT_GT(frame_count, 0) << "No frames counted";

    printf("Received %d frames, stream %u, pts %" PRIu64 "\n",
           frame_count, last_frame_hdr.stream_id, last_frame_hdr.pts_usec);

    dlclose(handle);
}



int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
