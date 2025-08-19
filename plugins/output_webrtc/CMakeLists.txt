# Output WebRTC Plugin CMakeLists.txt

# Use FFmpeg settings from parent
include_directories(${ZM_FFMPEG_INCLUDES})
link_directories(${ZM_FFMPEG_LIBDIRS})

# Find LibDataChannel (installed via vcpkg)
find_package(LibDataChannel REQUIRED)

# Find Boost
find_package(Boost REQUIRED COMPONENTS system thread)

# Find nlohmann_json
find_package(nlohmann_json REQUIRED)

# Create the shared library
add_library(output_webrtc SHARED output_webrtc.cpp)

# Set target properties
set_target_properties(output_webrtc PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    PREFIX ""
)

# Include directories
target_include_directories(output_webrtc PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${Boost_INCLUDE_DIRS}
)

# Link libraries
target_link_libraries(output_webrtc
    LibDataChannel::LibDataChannel
    ${Boost_LIBRARIES}
    nlohmann_json::nlohmann_json
    ${ZM_FFMPEG_LIBS}
    pthread
)

# Install the plugin
install(TARGETS output_webrtc
    LIBRARY DESTINATION plugins/output_webrtc
)
