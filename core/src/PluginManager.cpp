// PluginManager implements dynamic loading of C plugins via C ABI

// filepath: /Users/stevengilvarry/Code/zm-next/core/src/PluginManager.cpp
#include "zm/PluginManager.hpp"
#include "zm_plugin.h"
#include "zm/EventBus.hpp"
#include <dlfcn.h>
#include <iostream>
#include <cstring>

// Global host API for v1 plugins
zm_host_api_t gHost = {
    /* log */ [](void*, zm_log_level_t level, const char* msg) {
        const char* lvl = "INFO";
        if (level == ZM_LOG_DEBUG) lvl = "DEBUG";
        else if (level == ZM_LOG_WARN) lvl = "WARN";
        else if (level == ZM_LOG_ERROR) lvl = "ERROR";
        std::cerr << "[PLUGIN][" << lvl << "] " << (msg ? msg : "(null)") << std::endl;
    },
    /* publish_evt */ [](void* host_ctx, const char* json_event) -> void {
        zm::EventBus::instance().publish("plugin_event", json_event);
    },
    /* on_frame    */ nullptr,
    /* reserved    */ {nullptr, nullptr, nullptr, nullptr}
};

namespace zm {

PluginManager::PluginManager() {
    // Set up global host API for plugins (log and on_frame can be set elsewhere)
    gHost.publish_evt = [](void* host_ctx, const char* json_event) -> void {
        zm::EventBus::instance().publish("plugin_event", json_event);
    };
}

PluginManager::~PluginManager() {
    for (auto &handle : handles_) {
        dlclose(handle);
    }
}


// Legacy: load a single plugin (for tests)
bool PluginManager::loadPlugin(const std::string &path) {
    void *handle = dlopen(path.c_str(), RTLD_NOW);
    if (!handle) {
        std::cerr << "Failed to load plugin: " << dlerror() << std::endl;
        return false;
    }
    handles_.push_back(handle);
    return true;
}

// Load a pipeline: vector of PluginConfig (path, config_json)
bool PluginManager::loadPipeline(const std::vector<PluginConfig>& pipeline) {
    pipeline_.clear();
    for (const auto& pcfg : pipeline) {
        void* handle = dlopen(pcfg.path.c_str(), RTLD_NOW);
        if (!handle) {
            std::cerr << "Failed to load plugin: " << pcfg.path << ": " << dlerror() << std::endl;
            return false;
        }
        using init_fn_t = void(*)(zm_plugin_t*);
        auto init_fn = (init_fn_t)dlsym(handle, "zm_plugin_init");
        if (!init_fn) {
            std::cerr << "zm_plugin_init not found in " << pcfg.path << std::endl;
            dlclose(handle);
            return false;
        }
        PluginInstance inst;
        inst.handle = handle;
        std::memset(&inst.plugin, 0, sizeof(zm_plugin_t));
        init_fn(&inst.plugin);
        inst.config = pcfg;
        pipeline_.push_back(inst);
    }
    return true;
}

void PluginManager::startAll() {
    if (pipeline_.empty()) return;

    // Identify input plugin (first with type == ZM_PLUGIN_INPUT)
    size_t inputIdx = 0;
    for (; inputIdx < pipeline_.size(); ++inputIdx) {
        if (pipeline_[inputIdx].plugin.type == ZM_PLUGIN_INPUT)
            break;
    }
    if (inputIdx == pipeline_.size()) {
        std::cerr << "PluginManager::startAll: No input plugin found in pipeline." << std::endl;
        return;
    }

    // Prepare outputs (all plugins after input, or all non-inputs)
    std::vector<zm_plugin_t*> outputs;
    for (size_t i = 0; i < pipeline_.size(); ++i) {
        if (i != inputIdx)
            outputs.push_back(&pipeline_[i].plugin);
    }

    // Create ring buffer (256 slots, 1MiB each)
    ring_ = std::make_unique<ShmRing>(256, 1024*1024);
    // Start capture thread for input plugin
    captureThread_ = std::make_unique<CaptureThread>(&pipeline_[inputIdx].plugin, *ring_, outputs);
    captureThread_->start();

    // Start output/process plugins directly
    for (size_t i = 0; i < pipeline_.size(); ++i) {
        if (i == inputIdx) continue;
        auto& inst = pipeline_[i];
        if (inst.plugin.start) {
            inst.plugin.start(&inst.plugin, &gHost, nullptr, inst.config.config_json.c_str());
        }
    }
}

void PluginManager::stopAll() {
    for (auto& inst : pipeline_) {
        if (inst.plugin.stop) {
            inst.plugin.stop(&inst.plugin);
        }
    }
}


size_t PluginManager::pluginCount() const {
    return pipeline_.size();
}


void* PluginManager::getHandle(size_t index) const {
    if (index < pipeline_.size())
        return pipeline_[index].handle;
    return nullptr;
}

} // namespace zm

