cmake_minimum_required(VERSION 3.16)
project(review_export LANGUAGES CXX)

find_package(nlohmann_json REQUIRED)

# FFmpeg from the parent cached vars; libswscale (for the shared JPEG encoder in
# plugins/common) is not part of ZM_FFMPEG_LIBS.
include_directories(${ZM_FFMPEG_INCLUDES})
link_directories(${ZM_FFMPEG_LIBDIRS})
find_package(PkgConfig REQUIRED)
pkg_check_modules(SWSCALE REQUIRED libswscale)

if(APPLE)
    add_library(review_export SHARED review_export.cpp)
else()
    add_library(review_export MODULE review_export.cpp)
endif()

target_include_directories(review_export PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/plugins/common
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${SWSCALE_INCLUDE_DIRS}
)
target_link_directories(review_export PRIVATE ${ZM_FFMPEG_LIBDIRS} ${SWSCALE_LIBRARY_DIRS})
target_link_libraries(review_export PRIVATE
    ${ZM_FFMPEG_LIBS}
    ${SWSCALE_LIBRARIES}
    zmcore
    nlohmann_json::nlohmann_json
)

set_target_properties(review_export PROPERTIES
    PREFIX ""
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    OUTPUT_NAME "review_export"
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)
target_compile_options(review_export PRIVATE "-fvisibility=hidden")

if(APPLE)
    set_target_properties(review_export PROPERTIES
        INSTALL_RPATH "@loader_path;/opt/homebrew/lib;/usr/local/lib")
elseif(UNIX)
    set_target_properties(review_export PROPERTIES INSTALL_RPATH "$ORIGIN:$ORIGIN/../..")
endif()

install(TARGETS review_export DESTINATION ${CMAKE_INSTALL_LIBDIR}/zoneminder/plugins)

# Unit tests for the pure matte/cutout helpers (no model/FFmpeg session needed).
add_subdirectory(tests)
