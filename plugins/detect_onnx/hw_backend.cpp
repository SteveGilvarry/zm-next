// hw_backend.cpp — the single make_backend() factory.
//
// Each accelerator lives in its own translation unit (hw_backend_cuda.cpp,
// hw_backend_vaapi.cpp, hw_backend_metal.mm, hw_backend_openvino.cpp) and exposes a
// narrow make_<x>_backend() constructor under its build guard. This file is ALWAYS
// compiled; it dispatches by name and only reaches a backend when its macro is
// defined, so the default (CUDA-only, or even nothing) build still links cleanly and
// the unsupported names fall through to nullptr — letting a caller fall back to CPU.
//
// Guards (must match the per-backend TUs and the CMake compile-definitions):
//   ZMP_WITH_CUDA              -> make_cuda_backend()      (defined globally by top CMake)
//   ZM_WITH_VAAPI              -> make_vaapi_backend()
//   __APPLE__ && ZM_WITH_METAL -> make_metal_backend()
//   ZM_WITH_OPENVINO           -> make_openvino_backend()

#include "hw_backend.hpp"

namespace zm::hw {

// Per-backend factory entry points. Declared here (rather than in the public header)
// so the contract header stays vendor-agnostic; each is defined in its own TU under
// the matching guard. Only the enabled ones are linked in.
#ifdef ZMP_WITH_CUDA
std::unique_ptr<HwBackend> make_cuda_backend();
#endif
#ifdef ZM_WITH_VAAPI
std::unique_ptr<HwBackend> make_vaapi_backend();
#endif
#if defined(__APPLE__) && defined(ZM_WITH_METAL)
std::unique_ptr<HwBackend> make_metal_backend();
#endif
#ifdef ZM_WITH_OPENVINO
std::unique_ptr<HwBackend> make_openvino_backend();
#endif
#ifdef ZM_WITH_VULKAN
std::unique_ptr<HwBackend> make_vulkan_backend();
#endif

std::unique_ptr<HwBackend> make_backend(const std::string& kind) {
#ifdef ZMP_WITH_CUDA
    if (kind == "cuda") return make_cuda_backend();
#endif
#ifdef ZM_WITH_VAAPI
    if (kind == "vaapi") return make_vaapi_backend();
#endif
#if defined(__APPLE__) && defined(ZM_WITH_METAL)
    if (kind == "metal") return make_metal_backend();
#endif
#ifdef ZM_WITH_OPENVINO
    if (kind == "openvino") return make_openvino_backend();
#endif
#ifdef ZM_WITH_VULKAN
    if (kind == "vulkan") return make_vulkan_backend();
#endif
    (void)kind;
    return nullptr;   // unavailable backend -> caller falls back (e.g. to the CPU path)
}

}  // namespace zm::hw
