#pragma once


#include <vector>
#include <string>
#include "zm_plugin.h"
#include "zm/CaptureThread.hpp"
#include "zm/ShmRing.hpp"

namespace zm {

// Manages dynamic loading and lifecycle of C plugins for a pipeline
struct PluginConfig {
    std::string path;
    std::string config_json;
    // Optionally: wiring info, instance name, etc.
};

class PluginManager {
public:
    PluginManager();
    ~PluginManager();

    // Load and dlopen a plugin at given path (legacy, for tests)
    bool loadPlugin(const std::string& path);

    // Load a pipeline: vector of PluginConfig (path, config, ...)
    bool loadPipeline(const std::vector<PluginConfig>& pipeline);

    // Start all plugins in the pipeline
    void startAll();
    // Stop all plugins in the pipeline
    void stopAll();

    // Number of loaded plugins
    size_t pluginCount() const;

    // Get the raw handle for the plugin at index
    void* getHandle(size_t index) const;

private:
    struct PluginInstance {
        void* handle;
        zm_plugin_t plugin;
        PluginConfig config;
    };
    std::vector<void*> handles_; // legacy
    std::vector<PluginInstance> pipeline_;
    // For main pipeline: manage ring and capture thread
    std::unique_ptr<class ShmRing> ring_;
    std::unique_ptr<class CaptureThread> captureThread_;
};

} // namespace zm
