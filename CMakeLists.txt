cmake_minimum_required(VERSION 3.16)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
project(zm-next-core LANGUAGES CXX)

# Include CTest for add_test / enable_testing support
include(CTest)
enable_testing()

# Core library
add_subdirectory(core)

# Plugins
add_subdirectory(plugins/capture_rtsp)
add_subdirectory(plugins/hello)
