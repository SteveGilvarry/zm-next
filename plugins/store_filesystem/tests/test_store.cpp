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
    auto init = (int (*)(zm_plugin_t*))dlsym(handle, "zm_plugin_init");
    ASSERT_TRUE(init);
    zm_plugin_t plug = {};
    ASSERT_EQ(init(&plug), 0);
    std::string cfg = std::string("{\"root\":\"") + tmpdir + "\",\"max_secs\":2,\"monitor_id\":1}";
    std::vector<std::string> events;
    auto event_cb = [&](const char* name, const char* data) {
        if (std::string(name) == "FileClosed") events.push_back(data);
    };
    void* inst = plug.start(cfg.c_str(), +[](const char* n, const char* d){ /* no-op */ });
    ASSERT_TRUE(inst);
    zm_frame_t frame = {};
    frame.data = (uint8_t*)"dummy";
    frame.size = 5;
    frame.pts = 0;
    frame.dts = 0;
    frame.key = 1;
    frame.hw_type = 0;
    for (int i = 0; i < 50; ++i) {
        frame.pts = frame.dts = i * 1000000;
        plug.on_frame(inst, &frame, +[](const char*, const char*){});
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
    }
    plug.stop(inst, +[](const char*, const char*){});
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
