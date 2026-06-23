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
find_package(nlohmann_json 3.2.0 REQUIRED)

# Canonical per-monitor stream socket wire protocol (byte-compatible with the
# ZoneMinder zmc producer and the zm-api consumer). Dependency-free; shared by
# the core (WorkerLink), the wl_dump tool, and the protocol unit test.
add_library(zm_stream_protocol STATIC src/stream_socket_protocol.cpp)
target_include_directories(zm_stream_protocol PUBLIC include)
target_compile_features(zm_stream_protocol PUBLIC cxx_std_20)
target_compile_options(zm_stream_protocol PRIVATE -Wall -Werror)

add_library(zmcore STATIC
    src/PluginManager.cpp
    src/host_api.cpp
    src/EventBus.cpp
    src/ShmRing.cpp
    src/PipelineLoader.cpp
    src/CaptureThread.cpp
    src/StageRunner.cpp
    src/plugin_utils.cpp
    src/WorkerLink.cpp
)

target_include_directories(zmcore PUBLIC include)

# C++20 and optimization flags
target_compile_features(zmcore PUBLIC cxx_std_20)
target_compile_options(zmcore PRIVATE -Wall -Werror)
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -flto")
target_link_libraries(zmcore PRIVATE Threads::Threads nlohmann_json::nlohmann_json)
# Canonical stream socket protocol — PUBLIC so consumers of zmcore (WorkerLink
# users) can use the wire types.
target_link_libraries(zmcore PUBLIC zm_stream_protocol)
# PUBLIC: ShmRing.hpp exposes Boost.Interprocess, so consumers (tests, plugins)
# need the Boost include too.
target_include_directories(zmcore PUBLIC ${Boost_INCLUDE_DIRS})

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
target_link_libraries(test_pipeloader PRIVATE zmcore GTest::gtest_main Threads::Threads)
add_test(NAME PipelineLoaderTest COMMAND $<TARGET_FILE:test_pipeloader>)

# Unit tests for StageRunner (per-stage threading + drop-queue)
add_executable(test_stagerunner tests/test_stagerunner.cpp)
target_link_libraries(test_stagerunner PRIVATE zmcore GTest::gtest_main Threads::Threads)
add_test(NAME StageRunnerTest COMMAND $<TARGET_FILE:test_stagerunner>)

# Unit tests for the canonical stream socket wire protocol (encode/decode).
add_executable(test_stream_protocol tests/test_stream_protocol.cpp)
target_link_libraries(test_stream_protocol PRIVATE zm_stream_protocol GTest::gtest_main)
add_test(NAME StreamProtocolTest COMMAND $<TARGET_FILE:test_stream_protocol>)

# Unit tests for WorkerLink (the per-monitor worker interface). Links zmcore.
add_executable(test_workerlink tests/test_workerlink.cpp)
target_link_libraries(test_workerlink PRIVATE zmcore zm_stream_protocol GTest::gtest_main Threads::Threads)
target_include_directories(test_workerlink PRIVATE ${Boost_INCLUDE_DIRS})
add_test(NAME WorkerLinkTest COMMAND $<TARGET_FILE:test_workerlink>)


