
cmake_minimum_required(VERSION 3.16)
project(motion_basic LANGUAGES CXX)



# Build a single plugin with the correct extension for the OS

# Build a single plugin with the correct extension for the OS, matching capture_rtsp logic
if(APPLE)
    add_library(motion_basic SHARED motion_basic.cpp)
else()
    add_library(motion_basic MODULE motion_basic.cpp)
endif()

target_include_directories(motion_basic PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${ZM_FFMPEG_INCLUDES}           # only for timestamp helper
    ${ZM_XSIMD_INCLUDES}
)
target_link_directories(motion_basic PRIVATE ${ZM_FFMPEG_LIBDIRS})
target_link_libraries(motion_basic PRIVATE ${ZM_FFMPEG_LIBS} zmcore)

set_target_properties(motion_basic PROPERTIES
    PREFIX ""
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    OUTPUT_NAME "motion_basic"
)
target_compile_options(motion_basic PRIVATE "-fvisibility=hidden")
target_compile_features(motion_basic PRIVATE cxx_std_20)

install(TARGETS motion_basic DESTINATION lib/zoneminder/plugins)

add_subdirectory(tests)
