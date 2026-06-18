cmake_minimum_required(VERSION 3.16)
project(test_snapshot_util LANGUAGES CXX)

add_executable(test_snapshot_util test_snapshot_util.cpp)

set_property(TARGET test_snapshot_util PROPERTY CXX_STANDARD 17)
set_property(TARGET test_snapshot_util PROPERTY CXX_STANDARD_REQUIRED ON)

target_include_directories(test_snapshot_util PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(test_snapshot_util PRIVATE
    GTest::gtest_main
)

add_test(NAME StoreSnapshotTest COMMAND $<TARGET_FILE:test_snapshot_util>)
