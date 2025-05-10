// PipelineLoader.hpp
#pragma once

#include <string>
#include <vector>

namespace zm {

class PipelineLoader {
public:
    // Construct with path to SQLite DB ("": default to "pipelines.db")
    // dbPath: path to SQLite DB, default "pipelines.db"
    PipelineLoader(const std::string& dbPath = "pipelines.db");
    ~PipelineLoader();

    // Load pipeline configuration for a given monitor ID
    // Returns true on success, false on error or missing pipeline
    bool load(int monitorId);

    // Retrieve loaded plugin instance paths
    // Returns the list of plugin shared-lib paths for the loaded monitor
    const std::vector<std::string>& getPluginPaths() const;

private:
    std::string dbPath_;
    std::vector<std::string> pluginPaths_;
};

} // namespace zm
