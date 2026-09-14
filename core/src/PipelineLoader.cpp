// PipelineLoader.cpp: parse a plugin pipeline from a JSON file pushed by the
// orchestrating daemon (zm-next has no DB connection).
#include "zm/PipelineLoader.hpp"

#include <functional>
#include <iostream>
#include <nlohmann/json.hpp>
#include "zm/platform.hpp"
#include <fstream>
#include <iterator>


using namespace zm;
namespace zm {


PipelineLoader::PipelineLoader(const std::string& path)
    : path_(path) {}

PipelineLoader::~PipelineLoader() {}

bool PipelineLoader::load() {
    try {
        // "-" reads the pipeline from stdin: zm-api delivers it in memory so the
        // camera credentials inside never touch disk.
        // Read one JSON value, not to EOF: the writer may keep the pipe open.
        std::string text;
        if (path_ == "-") {
            nlohmann::json root;
            std::cin >> root;
            text = root.dump();
        } else {
            std::ifstream f(path_);
            if (!f) {
                std::cerr << "Cannot open file: " << path_ << std::endl;
                return false;
            }
            text.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        }
        return loadText(text);
    } catch (const std::exception& e) {
        std::cerr << "Exception reading pipeline " << path_ << ": " << e.what() << std::endl;
        return false;
    }
}

bool PipelineLoader::loadText(const std::string& json_text) {
    pipeline_.clear();
    try {
        nlohmann::json root = nlohmann::json::parse(json_text);
        if (!root.is_object()) {
            std::cerr << "JSON root is not an object in " << path_ << std::endl;
            return false;
        }
        raw_json_ = root.dump();
        if (!root.contains("plugins")) {
            std::cerr << "\"plugins\" key not found in " << path_ << std::endl;
            return false;
        }
        const auto& arr = root["plugins"];
        if (!arr.is_array()) {
            std::cerr << "\"plugins\" is not an array in " << path_ << std::endl;
            return false;
        }
        // Recursively flatten plugins while preserving the tree topology:
        // each node records the flat-vector indices of its children, so the
        // engine can route frames stage-to-stage. Returns the node's index.
        std::function<int(const nlohmann::json&)> add_plugin;
        add_plugin = [&](const nlohmann::json& plugin) -> int {
            if (!plugin.is_object()) {
                std::cerr << "A plugin entry is not an object in " << path_ << std::endl;
                return -1;
            }
            PluginConfig pcfg;
            if (plugin.contains("path")) {
                pcfg.path = plugin["path"].get<std::string>();
            } else if (plugin.contains("kind")) {
                const auto kind = plugin["kind"].get<std::string>();
                pcfg.path = plugins_dir() + "/" + kind + "/" + kind + ZM_PLUGIN_EXT;
            }
            if (plugin.contains("config"))
                pcfg.config_json = plugin["config"].dump();
            else if (plugin.contains("cfg"))
                pcfg.config_json = plugin["cfg"].dump();
            if (plugin.contains("queue_depth") && plugin["queue_depth"].is_number_integer())
                pcfg.queue_depth = plugin["queue_depth"].get<int>();
            const int myIndex = static_cast<int>(pipeline_.size());
            pipeline_.push_back(std::move(pcfg));
            // Recurse into children, appending their indices to this node.
            if (plugin.contains("children") && plugin["children"].is_array()) {
                for (const auto& child : plugin["children"]) {
                    const int ci = add_plugin(child);
                    if (ci >= 0) pipeline_[myIndex].children.push_back(ci);
                }
            }
            return myIndex;
        };
        for (const auto& plugin : arr) add_plugin(plugin);

        // Backward-compat: a flat array (no node declares children) is treated
        // as a linear chain node[i] -> node[i+1].
        bool anyChildren = false;
        for (const auto& p : pipeline_) if (!p.children.empty()) { anyChildren = true; break; }
        if (!anyChildren && pipeline_.size() > 1) {
            for (size_t i = 0; i + 1 < pipeline_.size(); ++i)
                pipeline_[i].children.push_back(static_cast<int>(i + 1));
        }
        return !pipeline_.empty();
    } catch (const std::exception& e) {
        std::cerr << "Exception parsing JSON " << path_ << ": " << e.what() << std::endl;
        return false;
    } catch (...) {
        std::cerr << "Unknown error parsing JSON " << path_ << std::endl;
        return false;
    }
}


const std::vector<PluginConfig>& PipelineLoader::getPipeline() const {
    return pipeline_;
}

} // namespace zm

void zm::PipelineLoader::printProgress() const {
    std::cout << "[PipelineLoader] Progress log:" << std::endl;
    for (const auto& msg : progress_msgs_) std::cout << "  " << msg << std::endl;
}
