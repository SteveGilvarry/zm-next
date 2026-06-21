cmake_minimum_required(VERSION 3.16)
project(zmcore LANGUAGES CXX)

# NOTE: deliberately NOT adding include_directories("/opt/homebrew/include") here.
# A blanket Homebrew include lands at -I position 1 and shadows the vcpkg protobuf
# (5.29) that generated worker_link.pb.* with Homebrew's protobuf 35 (ABI-
# incompatible). Core gets nlohmann_json / Boost / SQLite from their imported
# targets instead.

# GTest comes from vcpkg (find_package called in the top-level CMakeLists so the
# imported GTest::gtest_main target is visible in every subdirectory). The old
# FetchContent(googletest) download was removed: it required network access at
# configure time and a second googletest build.
find_package(GTest CONFIG REQUIRED)

find_package(Boost REQUIRED)
find_package(Threads REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(nlohmann_json 3.2.0 REQUIRED)

# WorkerLink is the ONLY core file that includes the generated worker_link.pb.h.
# It is isolated into its own library that links the vcpkg protobuf (zm_proto) and
# vcpkg nlohmann_json but deliberately NOT Boost — Boost drags /opt/homebrew/include
# (and Homebrew's incompatible protobuf 35) onto the compile line, which would
# shadow the vcpkg protobuf (5.29) that generated the code. Keeping Boost off this
# TU leaves vcpkg protobuf as the only one on the path.
add_library(zm_worker_link STATIC src/WorkerLink.cpp)
target_include_directories(zm_worker_link PUBLIC include)
target_compile_features(zm_worker_link PUBLIC cxx_std_20)
if(NOT MSVC)
    target_compile_options(zm_worker_link PRIVATE -Wall -Werror)
endif()
target_link_libraries(zm_worker_link PUBLIC zm_proto
    PRIVATE Threads::Threads nlohmann_json::nlohmann_json)
# WorkerLink.cpp uses Winsock2 (AF_UNIX socket, WSAPoll, WSASend) on Windows.
if(WIN32)
    target_link_libraries(zm_worker_link PRIVATE ws2_32)
endif()

add_library(zmcore STATIC
    src/PluginManager.cpp
    src/host_api.cpp
    src/EventBus.cpp
    src/ShmRing.cpp
    src/PipelineLoader.cpp
    src/CaptureThread.cpp
    src/StageRunner.cpp
    src/plugin_utils.cpp
)

target_include_directories(zmcore PUBLIC include)

# C++20 and optimization flags
target_compile_features(zmcore PUBLIC cxx_std_20)
# GCC/Clang-only flags. MSVC uses its own defaults (set per-config by CMake) and
# would mis-handle -g/-O0/-march/-flto and flood on /Wall, so gate them out.
if(NOT MSVC)
    target_compile_options(zmcore PRIVATE -Wall -Werror)
    set(CMAKE_CXX_FLAGS_DEBUG "-g -O0")
    set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -flto")
endif()
target_link_libraries(zmcore PRIVATE Threads::Threads nlohmann_json::nlohmann_json)
# WorkerLink (and transitively the protobuf worker-link types) — PUBLIC so
# consumers of zmcore can use them.
target_link_libraries(zmcore PUBLIC zm_worker_link)
# PUBLIC: ShmRing.hpp exposes Boost.Interprocess, so consumers (tests, plugins)
# need the Boost include too. This does NOT affect zm_worker_link, which links
# zm_proto (not zmcore) and so stays free of Homebrew's protobuf.
target_include_directories(zmcore PUBLIC ${Boost_INCLUDE_DIRS})
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
# libdl is POSIX-only (PluginManager uses LoadLibrary on Windows); link it only there.
target_link_libraries(test_plugin_manager PRIVATE zmcore GTest::gtest_main Threads::Threads)
if(NOT WIN32)
    target_link_libraries(test_plugin_manager PRIVATE ${CMAKE_DL_LIBS})
endif()
add_test(NAME PluginManagerTest COMMAND $<TARGET_FILE:test_plugin_manager>)

# Unit tests for PipelineLoader
add_executable(test_pipeloader tests/test_pipeloader.cpp)
target_include_directories(test_pipeloader PRIVATE ${SQLite3_INCLUDE_DIRS})
target_link_libraries(test_pipeloader PRIVATE zmcore GTest::gtest_main Threads::Threads ${SQLite3_LIBRARIES})
add_test(NAME PipelineLoaderTest COMMAND $<TARGET_FILE:test_pipeloader>)

# Unit tests for StageRunner (per-stage threading + drop-queue)
add_executable(test_stagerunner tests/test_stagerunner.cpp)
target_link_libraries(test_stagerunner PRIVATE zmcore GTest::gtest_main Threads::Threads)
add_test(NAME StageRunnerTest COMMAND $<TARGET_FILE:test_stagerunner>)

# Unit tests for WorkerLink (the per-monitor protobuf worker interface). Link
# zm_worker_link (vcpkg protobuf, no Boost) NOT zmcore, so the generated header
# compiles against the matching protobuf and not Homebrew's.
add_executable(test_workerlink tests/test_workerlink.cpp)
target_link_libraries(test_workerlink PRIVATE zm_worker_link GTest::gtest_main Threads::Threads)
add_test(NAME WorkerLinkTest COMMAND $<TARGET_FILE:test_workerlink>)


