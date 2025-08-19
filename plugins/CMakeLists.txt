cmake_minimum_required(VERSION 3.16)
project(plugins)

# Add plugin subdirectories below
add_subdirectory(hello)
add_subdirectory(capture_rtsp_multi)
add_subdirectory(decode_ffmpeg)

add_subdirectory(zones)
add_subdirectory(motion_pixel_diff)
add_subdirectory(store_filesystem)
add_subdirectory(output_webrtc)
add_subdirectory(output_mse)
