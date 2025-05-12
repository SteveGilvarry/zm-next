
add_executable(test_motion test_motion.cpp)
target_include_directories(test_motion PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${ZM_XSIMD_INCLUDES}
)
target_link_libraries(test_motion PRIVATE zmcore)
target_compile_definitions(test_motion PRIVATE TEST_CMAKE_BINARY_DIR=\"${CMAKE_BINARY_DIR}\")
add_test(NAME motion_basic_test COMMAND test_motion)
