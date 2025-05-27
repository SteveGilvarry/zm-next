# output_webrtc plugin CMake configuration

# Find required packages
find_package(LibDataChannel CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

# Platform-specific library type
if(APPLE)
    set(WEBRTC_LIB_TYPE SHARED)
else()
    set(WEBRTC_LIB_TYPE MODULE)
endif()

# Create the plugin library
add_library(output_webrtc ${WEBRTC_LIB_TYPE} output_webrtc.cpp)

# Set C++ standard
set_property(TARGET output_webrtc PROPERTY CXX_STANDARD 20)
set_property(TARGET output_webrtc PROPERTY CXX_STANDARD_REQUIRED ON)

# Enable media features for libdatachannel
target_compile_definitions(output_webrtc PRIVATE RTC_ENABLE_MEDIA=1)

# Include directories
target_include_directories(output_webrtc PRIVATE
    ${ZM_FFMPEG_INCLUDES}
    ${CMAKE_SOURCE_DIR}/core/include
)

# Link libraries - find FFmpeg components individually
find_library(AVFORMAT_LIB avformat PATHS ${ZM_FFMPEG_LIBDIRS})
find_library(AVCODEC_LIB avcodec PATHS ${ZM_FFMPEG_LIBDIRS})
find_library(AVUTIL_LIB avutil PATHS ${ZM_FFMPEG_LIBDIRS})

target_link_libraries(output_webrtc 
    LibDataChannel::LibDataChannel
    nlohmann_json::nlohmann_json
    ${AVFORMAT_LIB}
    ${AVCODEC_LIB}
    ${AVUTIL_LIB}
)

# Set library properties
set_target_properties(output_webrtc PROPERTIES
    PREFIX ""
    INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}"
    BUILD_WITH_INSTALL_RPATH TRUE
)

# Install the plugin
install(TARGETS output_webrtc
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/zoneminder/plugins
)

# Test executable
add_executable(test_webrtc tests/test_webrtc.cpp)

# Enable media features for test as well  
target_compile_definitions(test_webrtc PRIVATE RTC_ENABLE_MEDIA=1)

target_include_directories(test_webrtc PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
)

target_link_libraries(test_webrtc
    nlohmann_json::nlohmann_json
    gtest
    gtest_main
)

# Register the test
add_test(NAME webrtc COMMAND test_webrtc)
