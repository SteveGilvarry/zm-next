#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * MSE (Media Source Extensions) Plugin C API
 * 
 * This API allows external applications (such as Rust frontends) to:
 * - Register/unregister camera streams
 * - Pull media segments for streaming to browsers via MSE
 * - Get buffer statistics
 */

// =============================================================================
// STREAM MANAGEMENT
// =============================================================================

/**
 * Register a new camera stream for MSE processing
 * @param camera_id Unique camera identifier
 * @param stream_id Stream identifier (0 for main stream, 1+ for additional streams)
 * @param codec Codec name (e.g., "h264", "aac")
 * @param width Video width (0 if not applicable)
 * @param height Video height (0 if not applicable)
 */
void zm_mse_register_stream(uint32_t camera_id, uint32_t stream_id, const char* codec, int width, int height);

/**
 * Unregister a camera stream
 * @param camera_id Camera identifier
 * @param stream_id Stream identifier
 */
void zm_mse_unregister_stream(uint32_t camera_id, uint32_t stream_id);

// =============================================================================
// SEGMENT ACCESS
// =============================================================================

/**
 * Push a media segment to the buffer (typically called by the pipeline)
 * @param camera_id Camera identifier
 * @param data Segment data (H.264 NAL units, fragmented MP4, etc.)
 * @param size Size of the segment data in bytes
 */
void zm_mse_push_segment(uint32_t camera_id, const uint8_t* data, size_t size);

/**
 * Pop the next available segment (blocking call)
 * @param camera_id Camera identifier
 * @param out Buffer to write segment data to
 * @param max_size Maximum size of the output buffer
 * @return Number of bytes written to the output buffer, 0 if error
 */
size_t zm_mse_pop_segment(uint32_t camera_id, uint8_t* out, size_t max_size);

/**
 * Try to pop the next available segment (non-blocking)
 * @param camera_id Camera identifier
 * @param out Buffer to write segment data to
 * @param max_size Maximum size of the output buffer
 * @return Number of bytes written to the output buffer, 0 if no data available or error
 */
size_t zm_mse_try_pop_segment(uint32_t camera_id, uint8_t* out, size_t max_size);

// =============================================================================
// BUFFER STATISTICS
// =============================================================================

/**
 * Get the current number of segments in the buffer for a camera
 * @param camera_id Camera identifier
 * @return Number of segments currently buffered
 */
size_t zm_mse_get_buffer_size(uint32_t camera_id);

/**
 * Get extended buffer statistics for a camera
 * @param camera_id Camera identifier
 * @param total_segments_received Total number of segments received (lifetime)
 * @param dropped_segments Number of segments dropped due to buffer overflow
 * @return Current buffer size, or 0 if camera not found
 */
size_t zm_mse_get_buffer_stats(uint32_t camera_id, uint64_t* total_segments_received, uint64_t* dropped_segments);

/**
 * Get total bytes received for a camera stream
 * @param camera_id Camera identifier  
 * @return Total bytes received, or 0 if camera not found
 */
uint64_t zm_mse_get_bytes_received(uint32_t camera_id);

/**
 * Get frame count for a camera stream
 * @param camera_id Camera identifier
 * @return Total frame count, or 0 if camera not found
 */
uint64_t zm_mse_get_frame_count(uint32_t camera_id);

/**
 * Get initialization segment (init.mp4) for MSE
 * @param camera_id Camera identifier
 * @param out Buffer to write initialization segment to
 * @param max_size Maximum size of the output buffer  
 * @return Number of bytes written to the output buffer, 0 if no init segment available
 */
size_t zm_mse_get_init_segment(uint32_t camera_id, uint8_t* out, size_t max_size);

/**
 * Get the latest media segment without removing it from buffer
 * @param camera_id Camera identifier
 * @param out Buffer to write segment data to
 * @param max_size Maximum size of the output buffer
 * @return Number of bytes written to the output buffer, 0 if no segments available
 */
size_t zm_mse_get_latest_segment(uint32_t camera_id, uint8_t* out, size_t max_size);

/**
 * Debug function: Get info about all registered streams  
 * @param camera_ids Buffer to write camera IDs to
 * @param max_cameras Maximum number of camera IDs to return
 * @return Number of camera IDs written
 */
size_t zm_mse_get_active_cameras(uint32_t* camera_ids, size_t max_cameras);

#ifdef __cplusplus
}
#endif
