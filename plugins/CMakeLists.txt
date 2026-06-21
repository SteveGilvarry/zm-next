cmake_minimum_required(VERSION 3.16)
project(plugins)

# Phase-1 Windows bring-up builds a minimal, dependency-light plugin subset that
# exercises a full capture→decode→motion→detect pipeline plus the PluginManager
# test (hello). The remaining plugins pull heavier deps (curl, mqtt, opencv, face
# libs, extra FFmpeg filters) and are enabled with -DZM_ALL_PLUGINS=ON once the
# core build + GPU detect path are proven.
option(ZM_ALL_PLUGINS "Build the full plugin set (not just the Phase-1 subset)" OFF)

# --- Phase-1 core subset -----------------------------------------------------
add_subdirectory(hello)
add_subdirectory(decode_ffmpeg)
add_subdirectory(motion_pixel_diff)
add_subdirectory(capture_file)
add_subdirectory(detect_onnx)

# --- Full set ----------------------------------------------------------------
if(ZM_ALL_PLUGINS)
    add_subdirectory(capture_rtsp_multi)
    add_subdirectory(decode_detect)
    add_subdirectory(encode_ffmpeg)
    add_subdirectory(privacy_mask)
    add_subdirectory(overlay)
    add_subdirectory(zones)
    add_subdirectory(store_filesystem)
    add_subdirectory(output_webrtc)
    add_subdirectory(output_mse)
    add_subdirectory(motion_gate)
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
    add_subdirectory(store_event)
    add_subdirectory(store_snapshot)
endif()
