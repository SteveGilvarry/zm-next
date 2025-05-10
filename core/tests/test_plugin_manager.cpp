#include <gtest/gtest.h>
#include "zm/PluginManager.hpp"
#include "zm_plugin.h"
#include <dlfcn.h>
#include <filesystem>

using namespace zm;
using namespace std::filesystem;

#ifdef _WIN32
#define PLUGIN_EXT ".dll"
#else
#ifdef __APPLE__
#define PLUGIN_EXT ".dylib"
#else
#define PLUGIN_EXT ".so"
#endif
#endif

TEST(PluginManagerTest, LoadHelloPlugin) {
    PluginManager pm;
    // Construct path to hello plugin in build tree
    path pluginPath = path(TEST_CMAKE_BINARY_DIR) / "plugins" / "hello" / (std::string("hello") + PLUGIN_EXT);
    ASSERT_TRUE(exists(pluginPath)) << "Plugin not found at " << pluginPath;

    ASSERT_TRUE(pm.loadPlugin(pluginPath.string()));
    EXPECT_EQ(pm.pluginCount(), 1);

    // Retrieve handle and look up zm_plugin_init symbol
    void* handle = pm.getHandle(0);
    ASSERT_NE(handle, nullptr);
    using init_fn_t = void(*)(zm_plugin_t*);
    auto init_fn = (init_fn_t)dlsym(handle, "zm_plugin_init");
    ASSERT_NE(init_fn, nullptr) << dlerror();

    zm_plugin_t plugin;
    init_fn(&plugin);
    EXPECT_EQ(plugin.type, ZM_PLUGIN_OUTPUT);
    ASSERT_NE(plugin.instance, nullptr);

    // Test start/on_frame/stop
    zm_host_api_t host = {0};
    plugin.start(&plugin, &host, nullptr, "{}");
    // call on_frame twice
    plugin.on_frame(&plugin, nullptr, nullptr);
    plugin.on_frame(&plugin, nullptr, nullptr);
    plugin.stop(&plugin);

    // instance is int* counter
    int* counter = static_cast<int*>(plugin.instance);
    EXPECT_EQ(*counter, 2);

    // cleanup
    using cleanup_fn_t = void(*)(zm_plugin_t*);
    auto cleanup_fn = (cleanup_fn_t)dlsym(handle, "cleanup_plugin");
    ASSERT_NE(cleanup_fn, nullptr);
    cleanup_fn(&plugin);
}

// GoogleTest main
int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
