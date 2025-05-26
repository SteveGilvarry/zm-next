cmake_minimum_required(VERSION 3.16)
project(plugins)

# Add plugin subdirectories below
add_subdirectory(hello)
add_subdirectory(capture_rtsp)
add_subdirectory(capture_rtsp_multi)
add_subdirectory(decode_ffmpeg)

add_subdirectory(motion_basic)
add_subdirectory(store_filesystem)
