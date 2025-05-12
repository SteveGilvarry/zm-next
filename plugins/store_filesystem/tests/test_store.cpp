#include <gtest/gtest.h>
#include <zm_plugin.h>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>
#include <vector>
#include <cstdio>
#include <cstdlib>

namespace fs = std::filesystem;

static std::string get_lib_ext() {
#ifdef __APPLE__
    return ".dylib";
#else
    return ".so";
#endif
}

TEST(StoreFilesystem, SegmentAndEvent) {
    if (std::getenv("CI_NO_FFMPEG")) GTEST_SKIP();
    char tmpdir[] = "/tmp/zm_store_testXXXXXX";
    ASSERT_TRUE(mkdtemp(tmpdir));
    std::string lib = std::string(CMAKE_CURRENT_BINARY_DIR) + "/store_filesystem" + get_lib_ext();
    void* handle = dlopen(lib.c_str(), RTLD_NOW);
    ASSERT_TRUE(handle);
    using zm_plugin_init_fn = void (*)(zm_plugin_t*);
    auto init = (zm_plugin_init_fn)dlsym(handle, "zm_plugin_init");
    ASSERT_TRUE(init);
    zm_plugin_t plug = {};
    ASSERT_NO_THROW(init(&plug));
    std::string cfg = std::string("{\"root\":\"") + tmpdir + "\",\"max_secs\":2,\"monitor_id\":1}";
    std::vector<std::string> events;
    struct HostCtx { std::vector<std::string>* events; } host_ctx = { &events };
    zm_host_api_t host_api = {};
    host_api.publish_evt = [](void* ctx, const char* json_event) {
        auto* h = static_cast<HostCtx*>(ctx);
        h->events->push_back(json_event);
    };
    // Start plugin
    ASSERT_EQ(plug.start(&plug, &host_api, &host_ctx, cfg.c_str()), 0);
    // Simulate frames
    zm_frame_hdr_t hdr = {};
    hdr.hw_type = 0;
    hdr.bytes = 5;
    uint8_t payload[5] = {'d','u','m','m','y'};
    for (int i = 0; i < 50; ++i) {
        hdr.pts_usec = hdr.pts_usec + 1000000;
        uint8_t buf[sizeof(zm_frame_hdr_t) + sizeof(payload)];
        memcpy(buf, &hdr, sizeof(zm_frame_hdr_t));
        memcpy(buf + sizeof(zm_frame_hdr_t), payload, sizeof(payload));
        plug.on_frame(&plug, buf, sizeof(buf));
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
    }
    plug.stop(&plug);
    fs::path root(tmpdir);
    int mkv_count = 0;
    for (auto& p : fs::recursive_directory_iterator(root)) {
        if (p.path().extension() == ".mkv") ++mkv_count;
    }
    EXPECT_GE(mkv_count, 2);
    EXPECT_GE(events.size(), 1);
    fs::remove_all(root);
    dlclose(handle);
}
