cmake_minimum_required(VERSION 3.16)
project(motion_pixel_diff LANGUAGES CXX)

# Find required dependencies
find_package(Boost 1.70 REQUIRED)
find_package(nlohmann_json REQUIRED)

# libswscale (for the background-plate JPEG encoder in plugins/common); not part
# of the parent's ZM_FFMPEG_LIBS.
find_package(PkgConfig REQUIRED)
pkg_check_modules(SWSCALE REQUIRED libswscale)

# Build a single plugin with the correct extension for the OS
if(APPLE)
    add_library(motion_pixel_diff SHARED motion_pixel_diff.cpp)
else()
    add_library(motion_pixel_diff MODULE motion_pixel_diff.cpp)
endif()

target_include_directories(motion_pixel_diff PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/plugins/common
    ${ZM_XSIMD_INCLUDES}
    ${Boost_INCLUDE_DIRS}
    ${SWSCALE_INCLUDE_DIRS}
)

target_link_directories(motion_pixel_diff PRIVATE ${ZM_FFMPEG_LIBDIRS} ${SWSCALE_LIBRARY_DIRS})
target_link_libraries(motion_pixel_diff PRIVATE
    ${ZM_FFMPEG_LIBS}
    ${SWSCALE_LIBRARIES}
    zmcore
    ${Boost_LIBRARIES}
    nlohmann_json::nlohmann_json
)

set_target_properties(motion_pixel_diff PROPERTIES
    PREFIX ""
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    OUTPUT_NAME "motion_pixel_diff"
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

target_compile_options(motion_pixel_diff PRIVATE "-fvisibility=hidden")
target_compile_definitions(motion_pixel_diff PRIVATE 
    BOOST_GEOMETRY_NO_ROBUSTNESS
)
