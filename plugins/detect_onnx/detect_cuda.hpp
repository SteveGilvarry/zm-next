#pragma once

// CUDA zero-copy inference path. Compiled ONLY when ZM_WITH_CUDA is enabled
// (Linux/NVIDIA). On other platforms this header declares nothing and the
// detector falls back to the CPU path. NOT validated on macOS — validate on a
// CUDA box (see docs/GPU_Pipeline.md).

#ifdef ZMP_WITH_CUDA

#include "detect_postprocess.hpp"
#include <onnxruntime_cxx_api.h>
#include <cstdint>
#include <string>
#include <vector>

namespace zm::detect {

// Launch the fused preprocessing kernel: sample a region (crop_x/crop_y origin,
// crop_w x crop_h dims) of the NV12 CUDA surface (Y at y_ptr/y_pitch, interleaved
// UV at uv_ptr/uv_pitch) into a planar normalized CHW float tensor (3*net*net) on
// the device, applying the same letterbox (scale/pad, 114/255 border, BT.601
// YUV->RGB, /255) as the CPU path. Defined in detect_cuda.cu. 0 == success.
int launch_nv12_to_chw(const uint8_t* y_ptr, int y_pitch,
                       const uint8_t* uv_ptr, int uv_pitch,
                       int crop_x, int crop_y, int crop_w, int crop_h,
                       float scale, int pad_x, int pad_y,
                       int net, float* d_out);

// Downsample the luma plane to a small sw x sh grid on-device (for cheap on-GPU
// motion diffing). Only the tiny grid is read back, never the full frame.
int launch_luma_grid(const uint8_t* y_ptr, int y_pitch, int w, int h,
                     int ds, int sw, int sh, uint8_t* d_grid);

// Full zero-copy GPU inference: preprocess the NV12 surface (optionally just a
// crop region) on-device, run the session via IoBinding bound to CUDA memory (no
// host readback of the image), and decode the NMS-free output into source-pixel
// boxes. A non-empty crop (crop_w>0 && crop_h>0) runs detection on just that
// region and maps boxes back to full-surface coordinates; the default (0s) runs
// the whole frame, so existing callers are unaffected.
std::vector<Box> cuda_infer_nv12(Ort::Session& session,
                                 const std::string& input_name,
                                 const std::string& output_name,
                                 uint64_t y_ptr, int y_pitch,
                                 uint64_t uv_ptr, int uv_pitch,
                                 int width, int height, int net,
                                 float conf_thr, const std::vector<int>& allow,
                                 int crop_x = 0, int crop_y = 0,
                                 int crop_w = 0, int crop_h = 0);

// Preprocess one NV12 surface (optionally a crop) into a thread-local device CHW
// tensor (3*net*net floats) and return that device pointer plus the letterbox used.
// For feeding the shared InferenceEngine: the caller preprocesses zero-copy here,
// then hands the tensor to the engine which batches and runs it. The buffer is
// reused per thread, so consume it before the next call on the same thread.
const float* cuda_preprocess_nv12(uint64_t y_ptr, int y_pitch, uint64_t uv_ptr, int uv_pitch,
                                  int width, int height, int net, Letterbox& out_lb,
                                  int crop_x = 0, int crop_y = 0, int crop_w = 0, int crop_h = 0);

// Result of the cheap on-GPU luma-diff motion check (bbox in full-frame pixels).
struct MotionRoi { bool active = false; int x = 0, y = 0, w = 0, h = 0; int changed = 0; };

// Compute a single merged motion bounding box from the surface's luma vs the
// previous frame's downsampled grid (updated in place). Only the grid crosses PCIe.
//
// Global-luma-jump suppression (A/B knob, default OFF): if luma_jump_thresh > 0,
// the mean of the current downsampled grid is compared against *prev_mean (which
// the caller owns and this fn updates in place, mirroring prev_grid). When the
// absolute mean difference exceeds luma_jump_thresh — i.e. a whole-scene exposure
// shift (headlights, auto-gain, IR-cut) rather than a real mover — the call
// returns an inactive MotionRoi for this frame. luma_jump_thresh = 0 (the default)
// disables the check entirely, so existing callers / benchmark numbers are
// byte-identical. Pass prev_mean = nullptr to opt out even when a threshold is set.
MotionRoi cuda_motion_bbox_cpudiff(uint64_t y_ptr, int y_pitch, int width, int height,
                           std::vector<uint8_t>& prev_grid,
                           int ds, int pix_thr, int min_changed,
                           int luma_jump_thresh = 0, float* prev_mean = nullptr);

// --- Fully GPU-resident motion gate (downsample + diff both on device) ---
//
// Verdict accumulated on the device by launch_luma_diff: changed-cell count +
// bbox (in grid cells) + luma sum (for the mean / global-luma-jump check). Only
// this ~24-byte struct is read back, so neither the frame NOR the grid crosses
// PCIe — unlike cuda_motion_bbox_cpudiff, which copies the grid back and diffs on the CPU.
struct MotionVerdict { int cnt; int minx; int miny; int maxx; int maxy; unsigned long long sum; };

// Diff two device-resident grids (sw*sh cells) on the GPU. d_verdict must be
// pre-initialized to {0, sw, sh, -1, -1, 0}. Defined in detect_cuda.cu. 0 == ok.
int launch_luma_diff(const uint8_t* d_cur, const uint8_t* d_prev,
                     int sw, int sh, int thr, void* d_verdict);

// Opaque per-stream device state (the prev grid + verdict buffer live on the
// device across calls). The caller owns one per stream; create in start(),
// destroy in stop().
struct GpuDiffState;
GpuDiffState* gpudiff_state_create();
void gpudiff_state_destroy(GpuDiffState* st);

// Same semantics and output as cuda_motion_bbox_cpudiff, but the downsample AND the diff
// run on the GPU and prev_grid stays on the device (ping-pong) — only the ~24B
// verdict is read back. For grids finer than ~ds=32 this is faster than the host
// diff and crosses orders of magnitude less PCIe (see bench/bench_motion_diff.cu).
MotionRoi cuda_motion_bbox_gpudiff(uint64_t y_ptr, int y_pitch, int width, int height,
                                    GpuDiffState* st, int ds, int pix_thr, int min_changed,
                                    int luma_jump_thresh = 0);

// Like cuda_motion_bbox_cpudiff but returns SEPARATE motion regions via connected
// components on the changed-cell grid (4-connectivity, overlapping boxes merged,
// capped to max_regions by area, each >= min_cells). Lets detection run on each
// distinct mover instead of one frame-spanning merged box.
//
// Global-luma-jump suppression (A/B knob, default OFF): identical semantics to
// cuda_motion_bbox_cpudiff above. If luma_jump_thresh > 0 and the current grid mean
// differs from *prev_mean by more than luma_jump_thresh, an empty region list is
// returned for this frame (whole-scene exposure change, not real motion). The
// prev_grid is still updated so differencing resumes cleanly next frame.
// luma_jump_thresh = 0 (default) disables the check, keeping existing callers and
// benchmark output byte-identical. prev_mean = nullptr also opts out.
std::vector<MotionRoi> cuda_motion_regions_cpudiff(uint64_t y_ptr, int y_pitch, int width, int height,
                                           std::vector<uint8_t>& prev_grid,
                                           int ds, int pix_thr, int min_cells, int max_regions,
                                           int luma_jump_thresh = 0, float* prev_mean = nullptr);

// Batched zero-copy ROI inference: preprocess every region's crop of the NV12
// surface into one [N,3,net,net] device tensor, run a single batched session
// pass, and decode each region's boxes back to full-surface coordinates. The
// model must accept a dynamic batch dimension. Returns all boxes (flattened).
std::vector<Box> cuda_infer_nv12_batch(Ort::Session& session,
                                       const std::string& input_name,
                                       const std::string& output_name,
                                       uint64_t y_ptr, int y_pitch,
                                       uint64_t uv_ptr, int uv_pitch,
                                       int full_w, int full_h,
                                       const std::vector<MotionRoi>& regions,
                                       int net, float conf_thr,
                                       const std::vector<int>& allow);

}  // namespace zm::detect

#endif  // ZMP_WITH_CUDA
