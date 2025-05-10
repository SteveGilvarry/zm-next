// PluginManager implements dynamic loading of C plugins via C ABI

#include "zm/PluginManager.hpp"
#include "zm_plugin.h"
#include "zm_plugin.h"
#include "zm/EventBus.hpp"
#include <dlfcn.h>
#include <iostream>

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

bool PluginManager::loadPlugin(const std::string &path) {
    void *handle = dlopen(path.c_str(), RTLD_NOW);
    if (!handle) {
        std::cerr << "Failed to load plugin: " << dlerror() << std::endl;
        return false;
    }
    handles_.push_back(handle);
    return true;
}

size_t PluginManager::pluginCount() const {
    return handles_.size();
}

void* PluginManager::getHandle(size_t index) const {
    return handles_.at(index);
}

} // namespace zm

