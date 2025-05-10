#pragma once

#include <vector>
#include <string>

namespace zm {

// Manages dynamic loading of C plugins
class PluginManager {
public:
    PluginManager();
    ~PluginManager();

    // Load and dlopen a plugin at given path
    bool loadPlugin(const std::string& path);

    // Number of loaded plugins
    size_t pluginCount() const;

    // Get the raw handle for the plugin at index
    void* getHandle(size_t index) const;

private:
    std::vector<void*> handles_;
};

} // namespace zm
