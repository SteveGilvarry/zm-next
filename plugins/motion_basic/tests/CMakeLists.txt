# Test as a standalone executable, do NOT link to the plugin (MODULE) library
add_executable(test_motion test_motion.cpp)
target_include_directories(test_motion PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${ZM_XSIMD_INCLUDES}
)
target_link_libraries(test_motion PRIVATE zmcore)
target_compile_definitions(test_motion PRIVATE PLUGIN_PATH=\"${CMAKE_BINARY_DIR}/plugins/motion_basic/motion_basic.so\")
add_test(NAME motion_basic_test COMMAND test_motion)
