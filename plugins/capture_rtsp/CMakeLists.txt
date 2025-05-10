# filepath: /Users/stevengilvarry/Code/zm-next/plugins/capture_rtsp/CMakeLists.txt
project(capture_rtsp LANGUAGES CXX)
find_package(PkgConfig REQUIRED)
pkg_check_modules(AVFORMAT REQUIRED libavformat>=61)
pkg_check_modules(AVCODEC REQUIRED libavcodec>=61)
pkg_check_modules(AVUTIL REQUIRED libavutil>=59)
pkg_check_modules(SWSCALE REQUIRED libswscale>=7)

# Ensure FFmpeg 7.x+ is available
if(NOT AVFORMAT_FOUND OR NOT AVCODEC_FOUND OR NOT AVUTIL_FOUND OR NOT SWSCALE_FOUND)
    message(FATAL_ERROR "Required FFmpeg 7.x+ libraries not found")
endif()

# Library search paths will be set per-target via target_link_directories
# (Removed global link_directories to ensure per-target control)

## Build the rtsp_in plugin from the new rtsp_in.cpp file
add_library(rtsp_in MODULE rtsp_in.cpp)

target_include_directories(rtsp_in PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${AVFORMAT_INCLUDE_DIRS}
    ${AVCODEC_INCLUDE_DIRS}
    ${AVUTIL_INCLUDE_DIRS}
    ${SWSCALE_INCLUDE_DIRS}
)

## Add pkg-config library dirs for linking
target_link_directories(rtsp_in PRIVATE
    ${AVFORMAT_LIBRARY_DIRS}
    ${AVCODEC_LIBRARY_DIRS}
    ${AVUTIL_LIBRARY_DIRS}
    ${SWSCALE_LIBRARY_DIRS}
)

## Link against FFmpeg libraries (names only; dirs added above)
target_link_libraries(rtsp_in PRIVATE
    ${AVFORMAT_LIBRARIES}
    ${AVCODEC_LIBRARIES}
    ${AVUTIL_LIBRARIES}
    ${SWSCALE_LIBRARIES}
)

## Set C++20 standard
target_compile_features(rtsp_in PRIVATE cxx_std_20)

# On macOS add rpath so Homebrew FFmpeg dylibs are found at runtime
if(APPLE)
    set_target_properties(rtsp_in PROPERTIES
        INSTALL_RPATH "@loader_path;/opt/homebrew/lib"
    )
endif()

# Remove 'lib' prefix so plugin file is named 'rtsp_in.so'/'dll'
set_target_properties(rtsp_in PROPERTIES PREFIX "")

# For backward compatibility, also build the older version (optional)
add_library(capture_rtsp SHARED capture_rtsp.cpp)

# Add pkg-config library dirs for linking (same as rtsp_in)
target_link_directories(capture_rtsp PRIVATE
    ${AVFORMAT_LIBRARY_DIRS}
    ${AVCODEC_LIBRARY_DIRS}
    ${AVUTIL_LIBRARY_DIRS}
    ${SWSCALE_LIBRARY_DIRS}
)

target_include_directories(capture_rtsp PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${AVFORMAT_INCLUDE_DIRS}
    ${AVCODEC_INCLUDE_DIRS}
    ${AVUTIL_INCLUDE_DIRS}
    ${SWSCALE_INCLUDE_DIRS}
)

## Link against FFmpeg libraries and core
target_link_libraries(capture_rtsp PRIVATE
    ${AVFORMAT_LIBRARIES}
    ${AVCODEC_LIBRARIES}
    ${AVUTIL_LIBRARIES}
    ${SWSCALE_LIBRARIES}
    zmcore
)

# Set C++20 standard for capture_rtsp too
target_compile_features(capture_rtsp PRIVATE cxx_std_20)

# Remove 'lib' prefix so plugin files are named 'capture_rtsp.so'/'dll'
set_target_properties(capture_rtsp PROPERTIES PREFIX "")
# On macOS add rpath so Homebrew FFmpeg dylibs are found at runtime
if(APPLE)
    set_target_properties(capture_rtsp PROPERTIES
        INSTALL_RPATH "@loader_path;/opt/homebrew/lib"
    )
endif()
