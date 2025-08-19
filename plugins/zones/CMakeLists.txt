cmake_minimum_required(VERSION 3.16)
project(zones LANGUAGES CXX)

# Find required Boost components
find_package(Boost 1.70 REQUIRED)
find_package(nlohmann_json REQUIRED)

# Build a single plugin with the correct extension for the OS
if(APPLE)
    add_library(zones SHARED zones.cpp)
else()
    add_library(zones MODULE zones.cpp)
endif()

target_include_directories(zones PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${Boost_INCLUDE_DIRS}
)

target_link_libraries(zones PRIVATE 
    zmcore
    ${Boost_LIBRARIES}
    nlohmann_json::nlohmann_json
)

set_target_properties(zones PROPERTIES
    PREFIX ""
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    OUTPUT_NAME "zones"
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

target_compile_options(zones PRIVATE "-fvisibility=hidden")
target_compile_definitions(zones PRIVATE 
    BOOST_GEOMETRY_NO_ROBUSTNESS
)
