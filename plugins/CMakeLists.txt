cmake_minimum_required(VERSION 3.16)
project(plugins)

# zm_add_plugin(<kind>): build plugins/<kind>, copy its <kind>.schema.json (if
# any) next to the built library, and record the kind for manifest.json. The
# worker reads the manifest and schemas at runtime for its hello and the
# describe_plugins command (docs/Worker_Control_Protocol.md), so only plugins
# built here are ever advertised, never stale libraries left in the build tree.
set(ZM_PLUGIN_KINDS "")
macro(zm_add_plugin kind)
    add_subdirectory(${kind})
    list(APPEND ZM_PLUGIN_KINDS ${kind})
    set(_zm_schema "${CMAKE_CURRENT_SOURCE_DIR}/${kind}/${kind}.schema.json")
    if(EXISTS "${_zm_schema}")
        configure_file("${_zm_schema}" "${CMAKE_CURRENT_BINARY_DIR}/${kind}/${kind}.schema.json" COPYONLY)
        install(FILES "${_zm_schema}" DESTINATION lib/zm/plugins/${kind})
    endif()
endmacro()

zm_add_plugin(hello)
zm_add_plugin(capture_rtsp_multi)
zm_add_plugin(decode_ffmpeg)
zm_add_plugin(decode_detect)

zm_add_plugin(encode_ffmpeg)
zm_add_plugin(privacy_mask)
zm_add_plugin(overlay)

zm_add_plugin(zones)
zm_add_plugin(motion_pixel_diff)
zm_add_plugin(store)

zm_add_plugin(capture_file)

# AI / Phase 3
zm_add_plugin(motion_gate)
zm_add_plugin(detect_onnx)
zm_add_plugin(detect_openvocab)
zm_add_plugin(detect_pose)
zm_add_plugin(detect_seg)
zm_add_plugin(recognize_face)
zm_add_plugin(lpr)
zm_add_plugin(tracker)
zm_add_plugin(analytics_rules)
zm_add_plugin(alert_policy)
zm_add_plugin(audio_detect)
zm_add_plugin(describe_vlm)
zm_add_plugin(llm_event_review)
zm_add_plugin(output_mqtt)
zm_add_plugin(output_webhook)
zm_add_plugin(store_snapshot)
zm_add_plugin(review_export)

# manifest.json: the plugin kinds this build produces, in build order.
set(_zm_manifest_entries "")
foreach(kind IN LISTS ZM_PLUGIN_KINDS)
    list(APPEND _zm_manifest_entries "    {\"kind\": \"${kind}\"}")
endforeach()
list(JOIN _zm_manifest_entries ",\n" _zm_manifest_body)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/manifest.json" "{\n  \"plugins\": [\n${_zm_manifest_body}\n  ]\n}\n")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/manifest.json" DESTINATION lib/zm/plugins)

# Schemas must match the plugin source both ways (tools/check_plugin_schemas.py).
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME PluginSchemaCheck
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/check_plugin_schemas.py)
endif()

# Shared header-only helpers in plugins/common (no library to build, tests only).
find_package(nlohmann_json REQUIRED)
add_executable(test_on_demand common/tests/test_on_demand.cpp)
target_include_directories(test_on_demand PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/common)
target_link_libraries(test_on_demand PRIVATE GTest::gtest_main nlohmann_json::nlohmann_json)
set_target_properties(test_on_demand PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
add_test(NAME OnDemandCommandTest COMMAND $<TARGET_FILE:test_on_demand>)
