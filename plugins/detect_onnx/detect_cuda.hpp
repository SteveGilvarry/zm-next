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

// Launch the fused preprocessing kernel: sample the NV12 CUDA surface (Y plane at
// y_ptr/y_pitch, interleaved UV plane at uv_ptr/uv_pitch) into a planar,
// normalized CHW float tensor (3*net*net) on the device, applying the same
// letterbox (scale/pad, 114/255 border, BT.601 YUV->RGB, /255) as the CPU path.
// Defined in detect_cuda.cu. Returns a cudaError_t-as-int (0 == success).
int launch_nv12_to_chw(const uint8_t* y_ptr, int y_pitch,
                       const uint8_t* uv_ptr, int uv_pitch,
                       int src_w, int src_h,
                       float scale, int pad_x, int pad_y,
                       int net, float* d_out);

// Full zero-copy GPU inference: preprocess the NV12 surface on-device, run the
// session via IoBinding bound to CUDA memory (no host readback of the image),
// and decode the NMS-free output into source-pixel boxes.
std::vector<Box> cuda_infer_nv12(Ort::Session& session,
                                 const std::string& input_name,
                                 const std::string& output_name,
                                 uint64_t y_ptr, int y_pitch,
                                 uint64_t uv_ptr, int uv_pitch,
                                 int width, int height, int net,
                                 float conf_thr, const std::vector<int>& allow);

}  // namespace zm::detect

#endif  // ZMP_WITH_CUDA
