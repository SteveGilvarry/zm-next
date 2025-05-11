
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
#ifndef PLUGIN_PATH
#define PLUGIN_PATH "build/plugins/motion_basic/motion_basic.so"
#endif
    // Load the plugin .so dynamically
    void* handle = dlopen(PLUGIN_PATH, RTLD_NOW);
    if (!handle) {
        std::cerr << "Failed to load plugin: " << dlerror() << " (tried: " << PLUGIN_PATH << ")" << std::endl;
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
    plugin.on_frame(&plugin, &hdr, frame0.data());
    plugin.on_frame(&plugin, &hdr, frame1.data());
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
