#pragma once


#include <vector>
#include <string>
#include "zm_plugin.h"
#include "zm/CaptureThread.hpp"
#include "zm/ShmRing.hpp"

#include <memory>

namespace zm {

class WorkerLink;   // optional media sink handed to the CaptureThread
class StageRunner;  // per-stage thread + bounded drop-queue

// Manages dynamic loading and lifecycle of C plugins for a pipeline
struct PluginConfig {
    std::string path;
    std::string config_json;
    // Indices (into the flattened pipeline vector) of this node's downstream
    // children, i.e. the plugins its output frames feed. Preserves the JSON tree
    // topology so the engine can route frames stage-to-stage instead of fanning
    // the captured frame flat to every plugin.
    std::vector<int> children;
    // Bounded input-queue depth for this stage's thread (drop-oldest when full).
    // Small for low-latency detectors; large for recorders that shouldn't drop.
    int queue_depth = 16;
};

class PluginManager {
public:
    PluginManager();
    ~PluginManager();

    // Load and dlopen a plugin at given path (legacy, for tests)
    bool loadPlugin(const std::string& path);

    // Load a pipeline: vector of PluginConfig (path, config, ...)
    bool loadPipeline(const std::vector<PluginConfig>& pipeline);

    // Optional worker link that the CaptureThread taps compressed media into.
    // Must be set before startAll().
    void setWorkerLink(WorkerLink* link) { link_ = link; }

    // Name of the shared-memory ring segment. Must be unique per running
    // instance (e.g. per monitor). Set before startAll().
    void setRingName(const std::string& name) { ringName_ = name; }

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
    WorkerLink* link_ = nullptr;  // not owned
    std::string ringName_ = "zm_shmring";
    // One StageRunner (thread + bounded drop-queue) per non-input plugin. Used as
    // the host_ctx for each plugin so host->on_frame routes to that stage's
    // children's queues, decoupling stages so a slow one can't stall the rest.
    std::vector<std::unique_ptr<StageRunner>> runners_;
};

} // namespace zm
