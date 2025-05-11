// test_decode.cpp - GTest for decode_ffmpeg plugin
#include <gtest/gtest.h>
#include "zm_plugin.h"
#include <dlfcn.h>
#include <vector>
#include <fstream>
#include <cstring>
#include <filesystem>
#include <atomic>
using namespace std::filesystem;

static std::vector<std::vector<uint8_t>> packets;
static std::atomic<int> yuv_frames{0};

static void stub_log(void*, zm_log_level_t lvl, const char* msg) { printf("[%d] %s\n", lvl, msg); }
static void stub_publish_evt(void*, const char* json) { printf("Event: %s\n", json); }
static void stub_on_frame(void*, const void* frame, size_t sz) {
    if (sz < sizeof(zm_frame_hdr_t)) return;
    const zm_frame_hdr_t* hdr = (const zm_frame_hdr_t*)frame;
    if (hdr->bytes > 0) yuv_frames++;
}

// Helper: load H264 packets from file (raw stream)
static void load_packets(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) {
        std::cerr << "ERROR: Could not open H264 packet file: " << path << std::endl;
    }
    ASSERT_TRUE(f.is_open());
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(f)), {});
    // Naive split on 0x00000001 start code
    size_t i = 0;
    while (i + 4 < buf.size()) {
        if (buf[i] == 0 && buf[i+1] == 0 && buf[i+2] == 0 && buf[i+3] == 1) {
            size_t j = i + 4;
            while (j + 4 < buf.size() && !(buf[j] == 0 && buf[j+1] == 0 && buf[j+2] == 0 && buf[j+3] == 1)) j++;
            packets.emplace_back(buf.begin() + i, buf.begin() + (j < buf.size() ? j : buf.size()));
            i = j;
        } else {
            i++;
        }
    }
}

TEST(DecodeFfmpeg, DecodesSampleH264) {
    // Load plugin
    std::string binDir = TEST_CMAKE_BINARY_DIR;
    path buildPath = path(binDir) / "plugins" / "decode_ffmpeg" / "decode_ffmpeg.so";
    path srcPath = path("plugins") / "decode_ffmpeg" / "decode_ffmpeg.so";
    path pluginPath;
    if (exists(buildPath)) pluginPath = buildPath;
    else if (exists(srcPath)) pluginPath = srcPath;
    else FAIL() << "Cannot find plugin at " << buildPath << " or " << srcPath;
    void* handle = dlopen(pluginPath.c_str(), RTLD_NOW);
    ASSERT_NE(handle, nullptr) << dlerror();
    using init_fn_t = void(*)(zm_plugin_t*);
    auto init_fn = (init_fn_t)dlsym(handle, "zm_plugin_init");
    ASSERT_NE(init_fn, nullptr) << dlerror();
    zm_plugin_t plugin;
    init_fn(&plugin);
    zm_host_api_t host = {0};
    host.log = stub_log;
    host.publish_evt = stub_publish_evt;
    host.on_frame = stub_on_frame;
    std::string config = "{\"threads\":1,\"scale\":\"orig\",\"hw_decode\":false}";
    int result = plugin.start(&plugin, &host, nullptr, config.c_str());
    ASSERT_EQ(result, 0);

    // Feed packets as frames (simulate zm_frame_hdr_t + payload)
    for (const auto& pkt : packets) {
        std::vector<uint8_t> buf(sizeof(zm_frame_hdr_t) + pkt.size());
        zm_frame_hdr_t* hdr = reinterpret_cast<zm_frame_hdr_t*>(buf.data());
        memset(hdr, 0, sizeof(zm_frame_hdr_t));
        hdr->bytes = pkt.size();
        memcpy(buf.data() + sizeof(zm_frame_hdr_t), pkt.data(), pkt.size());
        plugin.on_frame(&plugin, hdr, buf.data());
    }
    ASSERT_NE(plugin.instance, nullptr);
    // Load sample packets (absolute path)
    load_packets("/Users/stevengilvarry/Code/zm-next/plugins/decode_ffmpeg/tests/data/packet.h264");
    for (const auto& pkt : packets) {
        std::vector<uint8_t> buf(sizeof(zm_frame_hdr_t) + pkt.size());
        zm_frame_hdr_t* hdr = reinterpret_cast<zm_frame_hdr_t*>(buf.data());
        memset(hdr, 0, sizeof(zm_frame_hdr_t));
        hdr->bytes = pkt.size();
        memcpy(buf.data() + sizeof(zm_frame_hdr_t), pkt.data(), pkt.size());
        plugin.on_frame(&plugin, hdr, buf.data());
    }
    plugin.stop(&plugin);
    EXPECT_GT(yuv_frames.load(), 0) << "No YUV frames decoded";
    dlclose(handle);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
