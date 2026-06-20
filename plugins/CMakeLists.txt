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
add_subdirectory(store_filesystem)
add_subdirectory(output_webrtc)
add_subdirectory(output_mse)

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
add_subdirectory(store_event)
add_subdirectory(store_snapshot)
