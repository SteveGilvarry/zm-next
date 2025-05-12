#include "zm_plugin.h"
#include <cstring>
#include <cstdlib>


// Plugin callbacks (modern API)
static int hello_start(zm_plugin_t* plugin, zm_host_api_t* host, void* host_ctx, const char* json_cfg) {
    if (host && host->log) host->log(host_ctx, ZM_LOG_INFO, "hello plugin start");
    return 0;
}

static void hello_stop(zm_plugin_t* plugin) {
    zm_host_api_t* host = nullptr;
    void* host_ctx = nullptr;
    if (plugin && plugin->instance) {
        // Optionally retrieve host/ctx if you store them in instance
    }
    // Logging is optional here
}

// New single-buffer API
static void hello_on_frame(zm_plugin_t* plugin, const void* buf, size_t size) {
    int* counter = static_cast<int*>(plugin->instance);
    if (!counter || !buf || size < sizeof(zm_frame_hdr_t)) return;
    // Optionally parse header: const zm_frame_hdr_t* hdr = (const zm_frame_hdr_t*)buf;
    (*counter)++;
}

extern "C" void zm_plugin_init(zm_plugin_t* plugin) {
    // Allocate counter
    int* counter = (int*)std::malloc(sizeof(int));
    *counter = 0;

    plugin->type = ZM_PLUGIN_OUTPUT;
    plugin->instance = counter;
    plugin->start = hello_start;
    plugin->stop = hello_stop;
    plugin->on_frame = hello_on_frame;
}

extern "C" void cleanup_plugin(zm_plugin_t* plugin) {
    if (plugin->instance) {
        std::free(plugin->instance);
        plugin->instance = nullptr;
    }
}
