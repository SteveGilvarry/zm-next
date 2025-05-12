#include <gtest/gtest.h>
#include "zm/PipelineLoader.hpp"
#include <sqlite3.h>
#include <filesystem>
#include <cstdio>

using namespace zm;
using namespace std::filesystem;

TEST(PipelineLoaderTest, LoadFromFile) {
    // Create temporary SQLite file
    const std::string dbFile = "test_pipelines.db";
    if (exists(dbFile)) remove(dbFile.c_str());

    sqlite3* db;
    ASSERT_EQ(SQLITE_OK, sqlite3_open(dbFile.c_str(), &db));
    // Create schema
    const char* create_sql =
        "CREATE TABLE pipelines(id INTEGER PRIMARY KEY, monitor_id INTEGER);"
        "CREATE TABLE plugin_instances(id INTEGER PRIMARY KEY, pipeline_id INTEGER, path TEXT);";
    char* errmsg = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_exec(db, create_sql, nullptr, nullptr, &errmsg));
    if (errmsg) sqlite3_free(errmsg);

    // Insert pipeline and plugin instances
    ASSERT_EQ(SQLITE_OK, sqlite3_exec(db,
        "INSERT INTO pipelines(id, monitor_id) VALUES (1, 42);"
        "INSERT INTO plugin_instances(pipeline_id, path) VALUES (1, 'foo.so');"
        "INSERT INTO plugin_instances(pipeline_id, path) VALUES (1, 'bar.so');",
        nullptr, nullptr, &errmsg));
    if (errmsg) sqlite3_free(errmsg);
    sqlite3_close(db);

    // Load via PipelineLoader
    PipelineLoader loader(dbFile);
    ASSERT_TRUE(loader.load(42));
    const auto& pipeline = loader.getPipeline();
    ASSERT_EQ(pipeline.size(), 2);
    EXPECT_EQ(pipeline[0].path, "foo.so");
    EXPECT_EQ(pipeline[1].path, "bar.so");

    // Clean up
    remove(dbFile.c_str());
}

// main omitted; use gtest_main
