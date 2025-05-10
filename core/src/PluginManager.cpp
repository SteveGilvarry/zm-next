// PluginManager implements dynamic loading of C plugins via C ABI
#include "zm/PluginManager.hpp"
#include "zm_plugin.h"
#include <dlfcn.h>
#include <iostream>

namespace zm {

PluginManager::PluginManager() = default;

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

