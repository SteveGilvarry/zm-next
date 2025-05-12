
#include <xsimd/xsimd.hpp>
#include <cassert>
#include <cstring>
#include <vector>
#include <string>
#include <iostream>
#include <zm_plugin.h>
#include <dlfcn.h>

struct MockHostApi : zm_host_api_t {
    int evt_count = 0;
    std::string last_evt;
    MockHostApi() {
        memset(this, 0, sizeof(zm_host_api_t));
        this->publish_evt = [](void* ctx, const char* msg) {
            auto* self = static_cast<MockHostApi*>(ctx);
            ++self->evt_count;
            self->last_evt = msg;
        };
        this->log = [](void*, zm_log_level_t, const char*){};
    }
};

typedef void (*zm_plugin_init_fn)(zm_plugin_t*);

int main() {
    std::string binDir = TEST_CMAKE_BINARY_DIR;
#ifdef _WIN32
    std::string ext = ".dll";
#elif defined(__APPLE__)
    std::string ext = ".dylib";
#else
    std::string ext = ".so";
#endif
    std::string pluginPath = binDir + "/plugins/motion_basic/motion_basic" + ext;
    void* handle = dlopen(pluginPath.c_str(), RTLD_NOW);
    if (!handle) {
        std::cerr << "Failed to load plugin: " << dlerror() << " (tried: " << pluginPath << ")" << std::endl;
        return 1;
    }
    zm_plugin_init_fn init_fn = (zm_plugin_init_fn)dlsym(handle, "zm_plugin_init");
    if (!init_fn) {
        std::cerr << "Failed to find zm_plugin_init: " << dlerror() << std::endl;
        return 2;
    }
    MockHostApi host;
    zm_plugin_t plugin = {};
    init_fn(&plugin);
    int w = 640, h = 360;
    std::vector<uint8_t> frame0(w*h, 0);
    std::vector<uint8_t> frame1 = frame0;
    for (int y=100; y<132; ++y)
        for (int x=100; x<132; ++x)
            frame1[y*w+x] = 255;
    zm_frame_hdr_t hdr = {};
    hdr.stream_id = w; // Overload for width
    hdr.flags = h;     // Overload for height
    hdr.hw_type = 0;
    // Start plugin
    plugin.start(&plugin, &host, &host, "{\"threshold\":18,\"min_pixels\":800}");
    std::vector<uint8_t> buf0(sizeof(zm_frame_hdr_t) + frame0.size());
    memcpy(buf0.data(), &hdr, sizeof(zm_frame_hdr_t));
    memcpy(buf0.data() + sizeof(zm_frame_hdr_t), frame0.data(), frame0.size());
    plugin.on_frame(&plugin, buf0.data(), buf0.size());

    std::vector<uint8_t> buf1(sizeof(zm_frame_hdr_t) + frame1.size());
    memcpy(buf1.data(), &hdr, sizeof(zm_frame_hdr_t));
    memcpy(buf1.data() + sizeof(zm_frame_hdr_t), frame1.data(), frame1.size());
    plugin.on_frame(&plugin, buf1.data(), buf1.size());
    assert(host.evt_count == 1);
    assert(host.last_evt.find("pixels") != std::string::npos);
    size_t px = 0;
    auto p = host.last_evt.find("pixels");
    if (p != std::string::npos) px = std::atoi(host.last_evt.c_str()+p+8);
    assert(px >= 1024);
    plugin.stop(&plugin);
    std::cout << "motion_basic test passed\n";
    dlclose(handle);
    return 0;
}
