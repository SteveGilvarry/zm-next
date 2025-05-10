// PipelineLoader.cpp: load plugin pipeline config via SQLite
#include "zm/PipelineLoader.hpp"
#include <sqlite3.h>
#include <iostream>

namespace zm {

PipelineLoader::PipelineLoader(const std::string& dbPath)
    : dbPath_(dbPath.empty() ? "pipelines.db" : dbPath) {}

PipelineLoader::~PipelineLoader() {}

bool PipelineLoader::load(int monitorId) {
    pluginPaths_.clear();
    sqlite3* db = nullptr;
    int rc = sqlite3_open(dbPath_.c_str(), &db);
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
        pluginPaths_.push_back(reinterpret_cast<const char*>(text));
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return !pluginPaths_.empty();
}

const std::vector<std::string>& PipelineLoader::getPluginPaths() const {
    return pluginPaths_;
}

} // namespace zm
