add_subdirectory(tests)

enable_testing()

find_package(GTest REQUIRED)
include_directories(${GTEST_INCLUDE_DIRS})

add_executable(test_hello_world test_hello_world.cpp)
target_link_libraries(test_hello_world ${GTEST_LIBRARIES} pthread)

add_test(NAME test_hello_world COMMAND test_hello_world)