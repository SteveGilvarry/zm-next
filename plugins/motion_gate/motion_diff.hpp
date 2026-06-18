#pragma once

// Pure, dependency-free luma-diff helpers for the lightweight motion gate.
// Kept separate from the plugin so they can be unit-tested without the ABI.

#include <cstdint>
#include <vector>

namespace zm::motiongate {

enum class PixFmt { RGB24, GRAY8, YUV420P };

// Extract a downsampled luma image by sampling every `step`-th pixel in x and y.
// For GRAY8 / YUV420P the first w*h bytes are the luma plane; for RGB24 luma is
// computed as 0.299R + 0.587G + 0.114B (fixed-point). `out` is resized to dw*dh.
inline void downsample_luma(const uint8_t* buf, PixFmt fmt, int w, int h, int step,
                            std::vector<uint8_t>& out, int& dw, int& dh) {
    if (step < 1) step = 1;
    dw = (w + step - 1) / step;
    dh = (h + step - 1) / step;
    out.resize(static_cast<size_t>(dw) * dh);
    size_t oi = 0;
    for (int y = 0; y < h; y += step) {
        for (int x = 0; x < w; x += step) {
            uint8_t l;
            if (fmt == PixFmt::RGB24) {
                const uint8_t* p = buf + (static_cast<size_t>(y) * w + x) * 3;
                l = static_cast<uint8_t>((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
            } else {
                l = buf[static_cast<size_t>(y) * w + x];  // luma plane
            }
            out[oi++] = l;
        }
    }
}

// Count luma samples whose absolute difference exceeds `threshold`. Returns -1 if
// the two buffers differ in size (e.g. a resolution change).
inline int count_changed(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b,
                         int threshold) {
    if (a.size() != b.size()) return -1;
    int changed = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        int d = static_cast<int>(a[i]) - static_cast<int>(b[i]);
        if (d < 0) d = -d;
        if (d > threshold) ++changed;
    }
    return changed;
}

}  // namespace zm::motiongate
