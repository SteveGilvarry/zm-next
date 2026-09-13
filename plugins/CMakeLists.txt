cmake_minimum_required(VERSION 3.16)
project(plugins)

# Add plugin subdirectories below
add_subdirectory(hello)
add_subdirectory(capture_rtsp_multi)
add_subdirectory(decode_ffmpeg)
add_subdirectory(decode_detect)

add_subdirectory(encode_ffmpeg)
add_subdirectory(privacy_mask)
add_subdirectory(overlay)

add_subdirectory(zones)
add_subdirectory(motion_pixel_diff)
add_subdirectory(store)

add_subdirectory(capture_file)

# AI / Phase 3
add_subdirectory(motion_gate)
add_subdirectory(detect_onnx)
add_subdirectory(detect_openvocab)
add_subdirectory(detect_pose)
add_subdirectory(detect_seg)
add_subdirectory(recognize_face)
add_subdirectory(lpr)
add_subdirectory(tracker)
add_subdirectory(analytics_rules)
add_subdirectory(alert_policy)
add_subdirectory(audio_detect)
add_subdirectory(describe_vlm)
add_subdirectory(llm_event_review)
add_subdirectory(output_mqtt)
add_subdirectory(output_webhook)
add_subdirectory(store_snapshot)
add_subdirectory(review_export)

# Shared header-only helpers in plugins/common (no library to build, tests only).
find_package(nlohmann_json REQUIRED)
add_executable(test_on_demand common/tests/test_on_demand.cpp)
target_include_directories(test_on_demand PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/common)
target_link_libraries(test_on_demand PRIVATE GTest::gtest_main nlohmann_json::nlohmann_json)
set_target_properties(test_on_demand PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
add_test(NAME OnDemandCommandTest COMMAND $<TARGET_FILE:test_on_demand>)
