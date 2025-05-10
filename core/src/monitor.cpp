// Stub for monitor integration
// monitor.cpp: orchestrate pipeline execution per monitor
#include <iostream>
#include <dlfcn.h>
#include <vector>
#include "zm_plugin.h"
#include "zm/ShmRing.hpp"
#include "zm/PipelineLoader.hpp"
#include "zm/PluginManager.hpp"
#include "zm/CaptureThread.hpp"

namespace zm {

void startMonitor(int monitorId) {
    // Load plugin paths for this monitor
    PipelineLoader loader;
    if (!loader.load(monitorId)) {
        // Logging fallback: print to stderr if gHost is not available
        fprintf(stderr, "startMonitor: no pipeline configured for monitor\n");
        return;
    }
    auto paths = loader.getPluginPaths();
    // Load plugins
    PluginManager pm;
    std::vector<zm_plugin_t*> plugins;
    for (const auto &p : paths) {
        if (!pm.loadPlugin(p)) {
            fprintf(stderr, "startMonitor: failed to load plugin\n");
            continue;
        }
        void* handle = pm.getHandle(pm.pluginCount()-1);
        using init_fn_t = void(*)(zm_plugin_t*);
        auto init_fn = (init_fn_t)dlsym(handle, "init_plugin");
        if (!init_fn) {
            fprintf(stderr, "startMonitor: init_plugin not found\n");
            continue;
        }
        zm_plugin_t* plugin = new zm_plugin_t;
        init_fn(plugin);
        plugins.push_back(plugin);
    }
    // Identify input vs outputs
    zm_plugin_t* inputPlugin = nullptr;
    std::vector<zm_plugin_t*> outputs;
    for (auto pl : plugins) {
        if (pl->type == ZM_PLUGIN_INPUT)
            inputPlugin = pl;
        else
            outputs.push_back(pl);
    }
    if (!inputPlugin) {
        fprintf(stderr, "startMonitor: no INPUT plugin found\n");
        return;
    }
    // Create shared-memory ring (256 slots, 1MiB each)
    ShmRing ring(256, 1024*1024);
    // Start capture thread
    CaptureThread capture(inputPlugin, ring, outputs);
    capture.start();
}

} // namespace zm
