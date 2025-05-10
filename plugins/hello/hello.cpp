#include "zm_plugin.h"
#include <cstring>
#include <cstdlib>

// Plugin callbacks
static void hello_start(zm_plugin_t* plugin) {
    host_log("hello plugin start");
}

static void hello_stop(zm_plugin_t* plugin) {
    host_log("hello plugin stop");
}

static void hello_on_frame(zm_plugin_t* plugin, const zm_frame_hdr_t* hdr, const void* payload, size_t payload_size) {
    // Count frames in plugin-specific context
    int* counter = static_cast<int*>(plugin->instance);
    (*counter)++;
    host_log("hello plugin on_frame");
}

extern "C" void init_plugin(zm_plugin_t* plugin) {
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
