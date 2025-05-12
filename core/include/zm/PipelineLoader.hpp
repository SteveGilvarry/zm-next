// PipelineLoader.hpp
#pragma once
#include "zm/PluginManager.hpp" // for PluginConfig
#include <string>
#include <vector>

namespace zm {

class PipelineLoader {
public:
    // If isJson is true, treat path as JSON file, else as SQLite DB
    PipelineLoader(const std::string& path, bool isJson = false);
    ~PipelineLoader();

    // For DB: load by monitorId. For JSON: monitorId ignored.
    bool load(int monitorId = 0);

    // Get parsed pipeline (vector of PluginConfig)
    const std::vector<PluginConfig>& getPipeline() const;

    // Progress info for last load
    void printProgress() const;
private:
    std::string path_;
    bool isJson_;
    std::vector<PluginConfig> pipeline_;
    // For progress/debug
    std::vector<std::string> progress_msgs_;
};

} // namespace zm
