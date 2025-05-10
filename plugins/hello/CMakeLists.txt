cmake_minimum_required(VERSION 3.16)
project(hello_plugin LANGUAGES CXX)

# Build as shared library without "lib" prefix
add_library(hello SHARED hello.cpp)
set_target_properties(hello PROPERTIES PREFIX "" )

# Include core headers for zm_plugin.h
target_include_directories(hello PRIVATE ${CMAKE_SOURCE_DIR}/core/include)

## Link against host core to resolve host_log/publish_event
target_link_libraries(hello PRIVATE zmcore)
