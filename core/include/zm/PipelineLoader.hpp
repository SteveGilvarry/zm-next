// PipelineLoader.hpp
#pragma once
#include "zm/PluginManager.hpp" // for PluginConfig
#include <string>
#include <vector>

namespace zm {

// Loads a plugin pipeline from a JSON file. zm-next has no DB connection — the
// orchestrating daemon (zm-api) generates the pipeline config and pushes it to
// the worker as a JSON file via --pipeline.
class PipelineLoader {
public:
    explicit PipelineLoader(const std::string& path);
    ~PipelineLoader();

    // Parse the pipeline file into a flat vector of PluginConfig.
    bool load();

    // Get parsed pipeline (vector of PluginConfig)
    const std::vector<PluginConfig>& getPipeline() const;

    // Progress info for last load
    void printProgress() const;
private:
    std::string path_;
    std::vector<PluginConfig> pipeline_;
    // For progress/debug
    std::vector<std::string> progress_msgs_;
};

} // namespace zm
