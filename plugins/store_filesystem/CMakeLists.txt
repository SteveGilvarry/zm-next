cmake_minimum_required(VERSION 3.16)
project(store_filesystem LANGUAGES CXX)

# Use FFmpeg settings from parent
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
    add_library(store_filesystem SHARED store_filesystem.cpp)
else()
    add_library(store_filesystem MODULE store_filesystem.cpp)
endif()

set_target_properties(store_filesystem PROPERTIES
    PREFIX ""
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)
target_compile_options(store_filesystem PRIVATE "-fvisibility=hidden")
target_compile_features(store_filesystem PRIVATE cxx_std_20)
target_link_libraries(store_filesystem PRIVATE ${ZM_FFMPEG_LIBS} zmcore swscale)

# RPATH handling
if(APPLE)
    set_target_properties(store_filesystem PROPERTIES
        INSTALL_RPATH "@loader_path;/opt/homebrew/lib;/usr/local/lib"
    )
elseif(UNIX)
    set_target_properties(store_filesystem PROPERTIES
        INSTALL_RPATH "$ORIGIN:$ORIGIN/../.."
    )
endif()

install(TARGETS store_filesystem DESTINATION ${CMAKE_INSTALL_LIBDIR}/zoneminder/plugins)

add_subdirectory(tests)
