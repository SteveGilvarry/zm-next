// PipelineLoader.cpp: load plugin pipeline config via SQLite
#include "zm/PipelineLoader.hpp"

#include <sqlite3.h>
#include <iostream>
#include <nlohmann/json.hpp>
#include "zm/platform.hpp"
#include <fstream>


using namespace zm;
namespace zm {


PipelineLoader::PipelineLoader(const std::string& path, bool isJson)
    : path_(path), isJson_(isJson) {}

PipelineLoader::~PipelineLoader() {}

bool PipelineLoader::load(int monitorId) {
    pipeline_.clear();
    if (isJson_) {
        try {
            std::ifstream f(path_);
            if (!f) {
                std::cerr << "Cannot open file: " << path_ << std::endl;
                return false;
            }
            nlohmann::json root;
            f >> root;
            if (!root.is_object()) {
                std::cerr << "JSON root is not an object in " << path_ << std::endl;
                return false;
            }
            if (!root.contains("plugins")) {
                std::cerr << "\"plugins\" key not found in " << path_ << std::endl;
                return false;
            }
            const auto& arr = root["plugins"];
            if (!arr.is_array()) {
                std::cerr << "\"plugins\" is not an array in " << path_ << std::endl;
                return false;
            }
            // Recursively flatten plugins with children
            std::function<void(const nlohmann::json&)> add_plugin;
            add_plugin = [&](const nlohmann::json& plugin) {
                if (!plugin.is_object()) {
                    std::cerr << "A plugin entry is not an object in " << path_ << std::endl;
                    return;
                }
                PluginConfig pcfg;
                if (plugin.contains("path")) {
                    pcfg.path = plugin["path"].get<std::string>();
                } else if (plugin.contains("kind")) {
                    // Use build/plugins/ as the plugin path when running from build dir
                    pcfg.path = std::string("plugins/") + plugin["kind"].get<std::string>() + "/" + plugin["kind"].get<std::string>() + ZM_PLUGIN_EXT;
                }
                if (plugin.contains("config"))
                    pcfg.config_json = plugin["config"].dump();
                else if (plugin.contains("cfg"))
                    pcfg.config_json = plugin["cfg"].dump();
                pipeline_.push_back(pcfg);
                // Recursively process children if present
                if (plugin.contains("children")) {
                    const auto& children = plugin["children"];
                    if (children.is_array()) {
                        for (const auto& child : children) add_plugin(child);
                    }
                }
            };
            for (const auto& plugin : arr) add_plugin(plugin);
            return !pipeline_.empty();
        } catch (const std::exception& e) {
            std::cerr << "Exception parsing JSON " << path_ << ": " << e.what() << std::endl;
            return false;
        } catch (...) {
            std::cerr << "Unknown error parsing JSON " << path_ << std::endl;
            return false;
        }
    } else {
        // DB mode (legacy)
        sqlite3* db = nullptr;
        int rc = sqlite3_open(path_.c_str(), &db);
        if (rc != SQLITE_OK) {
            std::cerr << "Cannot open DB: " << sqlite3_errmsg(db) << std::endl;
            sqlite3_close(db);
            return false;
        }
        const char* sql =
            "SELECT pi.path FROM pipelines p "
            "JOIN plugin_instances pi ON pi.pipeline_id = p.id "
            "WHERE p.monitor_id = ?;";
        sqlite3_stmt* stmt = nullptr;
        rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            std::cerr << "Failed to prepare statement: " << sqlite3_errmsg(db) << std::endl;
            sqlite3_close(db);
            return false;
        }
        sqlite3_bind_int(stmt, 1, monitorId);
        while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
            const unsigned char* text = sqlite3_column_text(stmt, 0);
            PluginConfig pcfg;
            pcfg.path = reinterpret_cast<const char*>(text);
            pcfg.config_json = "{}";
            pipeline_.push_back(pcfg);
        }
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        return !pipeline_.empty();
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
