cmake_minimum_required(VERSION 3.16)
project(motion_hybrid LANGUAGES CXX)

# Find required Boost components
find_package(Boost 1.70 REQUIRED)

# Build a single plugin with the correct extension for the OS, matching motion_basic logic
if(APPLE)
    add_library(motion_hybrid SHARED motion_hybrid.cpp)
else()
    add_library(motion_hybrid MODULE motion_hybrid.cpp)
endif()

target_include_directories(motion_hybrid PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${ZM_FFMPEG_INCLUDES}
    ${ZM_XSIMD_INCLUDES}
    ${Boost_INCLUDE_DIRS}
)

target_link_directories(motion_hybrid PRIVATE ${ZM_FFMPEG_LIBDIRS})
target_link_libraries(motion_hybrid PRIVATE 
    ${ZM_FFMPEG_LIBS} 
    zmcore
    ${Boost_LIBRARIES}
)

set_target_properties(motion_hybrid PROPERTIES
    PREFIX ""
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    OUTPUT_NAME "motion_hybrid"
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

target_compile_options(motion_hybrid PRIVATE "-fvisibility=hidden")
target_compile_definitions(motion_hybrid PRIVATE 
    BOOST_GEOMETRY_NO_ROBUSTNESS
    BOOST_AVAILABLE
)
