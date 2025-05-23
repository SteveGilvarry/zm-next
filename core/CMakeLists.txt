
cmake_minimum_required(VERSION 3.16)
project(zmcore LANGUAGES CXX)

include_directories("/opt/homebrew/include")

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
find_package(nlohmann_json 3.2.0 REQUIRED)

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
target_compile_options(zmcore PRIVATE -Wall -Werror)
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -flto")
target_link_libraries(zmcore PRIVATE Threads::Threads nlohmann_json::nlohmann_json)
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
  

