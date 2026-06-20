#pragma once

// motion_persist.hpp — N-of-M temporal persistence for the GPU motion gate.
//
// A/B TUNING KNOB (Phase 0, default = pass-through / NEUTRAL).
//
// Header-only, no dependencies beyond the standard library, no CUDA. The motion
// kernels (cuda_motion_regions / cuda_motion_bbox in detect_cuda.{hpp,cpp}) stay
// stateless across frames; the per-stream ring of recent activity lives HERE, in
// caller-owned state, exactly like prev_grid / prev_mean.
//
// WHY: rain speckle, leaf flicker and sensor noise are *incoherent* frame to
// frame — a changed cell lights up once and moves. A real person/vehicle paints
// the SAME grid cell across many consecutive frames. Requiring a cell to be
// "changed" in at least N of the last M frames before it counts as motion rejects
// the incoherent noise while keeping coherent trajectories. This is the cheap,
// recall-safe weather win called out in the Motion Gating workstream
// (docs/Research_Motion_and_LLM_Review.md, tuning recipe item 3).
//
// DEFAULT IS NEUTRAL: construct with require_n <= 1 (or m <= 1) and every call is
// an identity pass-through — the input grid/regions are returned unchanged and no
// existing benchmark number moves. The knob only engages when the caller sets
// n >= 2.
//
// The utility operates on the GATE's per-cell changed mask (1 = changed this
// frame). Two entry points:
//   * update_mask(): in-place N-of-M filtering of a raw changed-cell mask, to be
//     applied BEFORE connected components. This is the most faithful place to
//     gate — it suppresses transient cells before they ever form a region.
//   * passes(): a region-level helper for callers that only have boxes (e.g. the
//     existing cuda_motion_regions output). It samples the region's cells against
//     the ring and reports whether enough of them are persistent.
//
// Because cuda_motion_regions currently returns only merged boxes (not the raw
// mask), the recommended Phase-0 wiring keeps the kernel untouched and applies
// MotionPersist at the region level in the caller (see WIRING below). A future
// refinement can expose the raw mask and use update_mask() for cell-exact gating.
//
// ------------------------------------------------------------------ WIRING -----
// bench_gpu_roi.cpp / the detect_onnx ROI cascade own one MotionPersist per
// stream alongside `prev` / `prevGrid`. Pseudocode (region-level, Phase 0):
//
//   struct Ctx {
//       std::vector<uint8_t> prev;                 // existing prev grid
//       zm::detect::MotionPersist persist{1, 1};   // NEUTRAL default (n=1,m=1)
//       // ...to A/B-enable: persist = MotionPersist{2, 3}; // 2-of-3
//   };
//
//   auto regions = zm::detect::cuda_motion_regions(yp, ypitch, w, h, c->prev,
//                      c->ds, c->thr, c->minchg, c->maxRegions,
//                      c->lumaJumpThresh, &c->prevMean);   // luma knob (also off by default)
//
//   // Roll this frame's regions into the ring and keep only persistent ones.
//   // With the default n=1 this returns `regions` unchanged.
//   regions = c->persist.filter_regions(regions, w / c->ds, h / c->ds, c->ds);
//
//   // ...regions then feed cuda_infer_nv12_batch exactly as today.
//
// For the cell-exact variant, the caller would instead obtain the raw changed
// mask, call persist.update_mask(mask, sw, sh), then run connected components.
// -------------------------------------------------------------------------------

#include <algorithm>
#include <cstdint>
#include <deque>
#include <vector>

namespace zm::detect {

struct MotionRoi;  // fwd decl; full definition in detect_cuda.hpp

// Ring of the last M per-cell changed masks; a cell "persists" if it was changed
// in at least N of them (counting the current frame, which is pushed first).
class MotionPersist {
public:
    // n = require_n, m = window_m. n <= 1 || m <= 1 => NEUTRAL pass-through.
    MotionPersist(int require_n = 1, int window_m = 1)
        : n_(require_n), m_(window_m < 1 ? 1 : window_m) {}

    bool enabled() const { return n_ >= 2 && m_ >= 2; }
    int require_n() const { return n_; }
    int window_m() const { return m_; }

    // Reset ring (e.g. on resolution change / stream restart).
    void reset() { ring_.clear(); cells_ = 0; }

    // Push the current frame's changed mask (size = sw*sh, 1=changed) into the
    // ring and rewrite it in place to keep ONLY cells changed in >= N of the last
    // M frames. Neutral default leaves the mask untouched.
    //
    // Returns the number of cells that survived (== popcount of mask after).
    int update_mask(std::vector<uint8_t>& mask, int sw, int sh) {
        const size_t cells = static_cast<size_t>(sw) * static_cast<size_t>(sh);
        if (mask.size() != cells) return -1;  // caller geometry mismatch
        if (!enabled()) {                      // NEUTRAL: identity
            int c = 0; for (uint8_t v : mask) c += (v != 0);
            return c;
        }
        if (cells_ != cells) reset();
        cells_ = cells;

        // Binarize and push.
        std::vector<uint8_t> bin(cells);
        for (size_t k = 0; k < cells; ++k) bin[k] = mask[k] ? 1 : 0;
        ring_.push_back(std::move(bin));
        while (static_cast<int>(ring_.size()) > m_) ring_.pop_front();

        // Per-cell count over the window; keep cells reaching N.
        int survived = 0;
        for (size_t k = 0; k < cells; ++k) {
            int cnt = 0;
            for (const auto& f : ring_) cnt += f[k];
            const uint8_t keep = (cnt >= n_) ? 1 : 0;
            mask[k] = keep;
            survived += keep;
        }
        return survived;
    }

    // Region-level helper for the Phase-0 wiring where only merged boxes are
    // available. Synthesizes a changed mask from the regions (cells covered by a
    // region box are marked changed), runs update_mask, then keeps only regions
    // that still overlap a persistent cell. Neutral default returns `regions`
    // unchanged (and pushes nothing, so toggling the knob later starts clean).
    //
    // sw/sh = downsampled grid dims (width/ds, height/ds); ds = the same
    // downscale passed to cuda_motion_regions.
    std::vector<MotionRoi> filter_regions(const std::vector<MotionRoi>& regions,
                                          int sw, int sh, int ds);

private:
    int n_, m_;
    size_t cells_ = 0;
    std::deque<std::vector<uint8_t>> ring_;
};

}  // namespace zm::detect

// Out-of-line definition kept in the header (still header-only) but AFTER
// MotionRoi is a complete type. Callers that need filter_regions must include
// detect_cuda.hpp (for MotionRoi) before instantiating it; the template-free body
// below only compiles where MotionRoi is complete. To keep this strictly
// header-only without ordering constraints, the body is guarded on the same CUDA
// switch as MotionRoi's definition.
#ifdef ZMP_WITH_CUDA
#include "detect_cuda.hpp"

namespace zm::detect {

inline std::vector<MotionRoi>
MotionPersist::filter_regions(const std::vector<MotionRoi>& regions,
                              int sw, int sh, int ds) {
    if (!enabled() || sw <= 0 || sh <= 0 || ds <= 0) return regions;  // NEUTRAL

    const size_t cells = static_cast<size_t>(sw) * static_cast<size_t>(sh);
    std::vector<uint8_t> mask(cells, 0);
    for (const MotionRoi& r : regions) {
        const int c0 = std::max(0, r.x / ds), r0 = std::max(0, r.y / ds);
        const int c1 = std::min(sw, (r.x + r.w + ds - 1) / ds);
        const int r1 = std::min(sh, (r.y + r.h + ds - 1) / ds);
        for (int j = r0; j < r1; ++j)
            for (int i = c0; i < c1; ++i) mask[static_cast<size_t>(j) * sw + i] = 1;
    }

    update_mask(mask, sw, sh);  // rewrites mask to persistent cells

    std::vector<MotionRoi> kept;
    kept.reserve(regions.size());
    for (const MotionRoi& r : regions) {
        const int c0 = std::max(0, r.x / ds), r0 = std::max(0, r.y / ds);
        const int c1 = std::min(sw, (r.x + r.w + ds - 1) / ds);
        const int r1 = std::min(sh, (r.y + r.h + ds - 1) / ds);
        bool persistent = false;
        for (int j = r0; j < r1 && !persistent; ++j)
            for (int i = c0; i < c1; ++i)
                if (mask[static_cast<size_t>(j) * sw + i]) { persistent = true; break; }
        if (persistent) kept.push_back(r);
    }
    return kept;
}

}  // namespace zm::detect
#endif  // ZMP_WITH_CUDA
