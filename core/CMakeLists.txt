cmake_minimum_required(VERSION 3.16)
project(zmcore LANGUAGES CXX)

# Core library
include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/release-1.12.0.zip
)
FetchContent_MakeAvailable(googletest)

find_package(Boost REQUIRED)
find_package(Threads REQUIRED)
find_package(SQLite3 REQUIRED)

add_library(zmcore STATIC
    src/PluginManager.cpp
    src/host_api.cpp
    src/EventBus.cpp
    src/ShmRing.cpp
    src/PipelineLoader.cpp
    src/monitor.cpp
    src/CaptureThread.cpp
)

target_include_directories(zmcore PUBLIC include)

# C++20 and optimization flags
target_compile_features(zmcore PUBLIC cxx_std_20)
target_compile_options(zmcore PRIVATE -O3 -march=native -flto -Wall -Werror)
target_link_libraries(zmcore PRIVATE Threads::Threads)
target_include_directories(zmcore PRIVATE ${Boost_INCLUDE_DIRS})
target_link_libraries(zmcore PRIVATE SQLite::SQLite3)

# Unit tests for ShmRing
enable_testing()
add_executable(test_shmring tests/test_shmring.cpp)
target_link_libraries(test_shmring PRIVATE zmcore GTest::gtest_main Threads::Threads)
target_include_directories(test_shmring PRIVATE ${Boost_INCLUDE_DIRS})
add_test(NAME ShmRingTest COMMAND $<TARGET_FILE:test_shmring>)
  
# Unit tests for EventBus
add_executable(test_eventbus tests/test_eventbus.cpp)
target_link_libraries(test_eventbus PRIVATE zmcore GTest::gtest_main Threads::Threads)
add_test(NAME EventBusTest COMMAND $<TARGET_FILE:test_eventbus>)
  
# Unit tests for PluginManager and hello plugin
add_executable(test_plugin_manager tests/test_plugin_manager.cpp)
# Pass CMake binary directory to test for plugin path resolution
target_compile_definitions(test_plugin_manager PRIVATE TEST_CMAKE_BINARY_DIR="${CMAKE_BINARY_DIR}")
target_link_libraries(test_plugin_manager PRIVATE zmcore GTest::gtest_main Threads::Threads dl)
add_test(NAME PluginManagerTest COMMAND $<TARGET_FILE:test_plugin_manager>)

# Unit tests for PipelineLoader
add_executable(test_pipeloader tests/test_pipeloader.cpp)
target_include_directories(test_pipeloader PRIVATE ${SQLite3_INCLUDE_DIRS})
target_link_libraries(test_pipeloader PRIVATE zmcore GTest::gtest_main Threads::Threads ${SQLite3_LIBRARIES})
add_test(NAME PipelineLoaderTest COMMAND $<TARGET_FILE:test_pipeloader>)

# Unit tests for monitor start
add_executable(test_monitor tests/test_monitor.cpp)
target_link_libraries(test_monitor PRIVATE zmcore GTest::gtest_main Threads::Threads)
add_test(NAME MonitorTest COMMAND $<TARGET_FILE:test_monitor>)
  
# Unit tests for capture_rtsp plugin
add_executable(test_capture_rtsp tests/test_capture_rtsp.cpp)
# Pass binary directory to tests for plugin lookup
target_compile_definitions(test_capture_rtsp PRIVATE TEST_CMAKE_BINARY_DIR="${CMAKE_BINARY_DIR}")
target_link_libraries(test_capture_rtsp PRIVATE zmcore GTest::gtest_main Threads::Threads dl)
add_test(NAME CaptureRtspPluginTest COMMAND $<TARGET_FILE:test_capture_rtsp>)
