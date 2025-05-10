#include <gtest/gtest.h>
#include "zm_plugin.h"
#include <dlfcn.h>
#include <filesystem>

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

// TEST_CMAKE_BINARY_DIR is defined via compile_definitions in CMake
#include <string>

TEST(CaptureRtspPluginTest, InitStartStop) {
    // Construct path to the plugin using the binary directory macro
    std::string binDir = TEST_CMAKE_BINARY_DIR;
    path pluginPath = path(binDir) / "plugins" / "capture_rtsp" / (std::string("capture_rtsp") + PLUGIN_EXT);
    ASSERT_TRUE(exists(pluginPath)) << "Cannot find plugin at " << pluginPath;

    void* handle = dlopen(pluginPath.c_str(), RTLD_NOW);
    ASSERT_NE(handle, nullptr) << dlerror();

    using init_fn_t = void(*)(zm_plugin_t*);
    auto init_fn = (init_fn_t)dlsym(handle, "init_plugin");
    ASSERT_NE(init_fn, nullptr) << dlerror();

    zm_plugin_t plugin;
    init_fn(&plugin);
    EXPECT_EQ(plugin.type, ZM_PLUGIN_INPUT);
    ASSERT_NE(plugin.instance, nullptr);

    plugin.start(&plugin);
    plugin.stop(&plugin);
    // Instance should be freed in stop
    EXPECT_EQ(plugin.instance, nullptr);

    dlclose(handle);
}
