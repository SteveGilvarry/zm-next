cmake_minimum_required(VERSION 3.16)
project(store LANGUAGES CXX)

find_package(nlohmann_json REQUIRED)

# Use FFmpeg settings from parent (cached by the top-level build).
include_directories(${ZM_FFMPEG_INCLUDES})
link_directories(${ZM_FFMPEG_LIBDIRS})

find_path(NLOHMANN_JSON_INCLUDE_DIR nlohmann/json.hpp
    PATHS /opt/homebrew/include /usr/local/include
)
if(NOT NLOHMANN_JSON_INCLUDE_DIR)
    message(FATAL_ERROR "nlohmann/json.hpp not found. Please install nlohmann-json.")
endif()
include_directories(${NLOHMANN_JSON_INCLUDE_DIR})

if(APPLE)
    add_library(store SHARED store.cpp)
else()
    add_library(store MODULE store.cpp)
endif()

set_target_properties(store PROPERTIES
    PREFIX ""
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    OUTPUT_NAME "store"
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)
target_include_directories(store PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(store PRIVATE "-fvisibility=hidden")
target_link_libraries(store PRIVATE ${ZM_FFMPEG_LIBS} zmcore nlohmann_json::nlohmann_json)

# RPATH handling
if(APPLE)
    set_target_properties(store PROPERTIES
        INSTALL_RPATH "@loader_path;/opt/homebrew/lib;/usr/local/lib"
    )
elseif(UNIX)
    set_target_properties(store PROPERTIES
        INSTALL_RPATH "$ORIGIN:$ORIGIN/../.."
    )
endif()

install(TARGETS store DESTINATION ${CMAKE_INSTALL_LIBDIR}/zoneminder/plugins)

# Unit tests for the pure trigger helper (no ABI / FFmpeg needed).
add_executable(test_store_trigger tests/test_event_trigger.cpp)
target_include_directories(test_store_trigger PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_store_trigger PRIVATE GTest::gtest_main)
set_target_properties(test_store_trigger PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
add_test(NAME StoreTriggerTest COMMAND $<TARGET_FILE:test_store_trigger>)
