# Find FFmpeg components needed for MP4 fragmentation
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavformat
    libavcodec
    libavutil
)

# Find Boost for networking
find_package(Boost REQUIRED COMPONENTS system thread)

# Find nlohmann/json
find_package(nlohmann_json REQUIRED)

add_library(output_mse SHARED output_mse.cpp)
target_include_directories(output_mse PRIVATE ${CMAKE_SOURCE_DIR}/core/include)
target_link_libraries(output_mse PRIVATE 
    zmcore 
    PkgConfig::FFMPEG 
    Boost::system 
    Boost::thread
    nlohmann_json::nlohmann_json
)
set_target_properties(output_mse PROPERTIES PREFIX "" SUFFIX ".dylib")
