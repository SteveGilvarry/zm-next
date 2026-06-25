#pragma once

// Pure, runtime-free matte helpers for review_export (motion synopsis): rasterise
// a seg polygon to an 8-bit matte, feather it, and premultiply an RGB crop by it.
// Kept header-only and FFmpeg-free so it can be unit-tested without a model.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace zm {
namespace review {

// Even-odd scanline fill of a polygon into an 8-bit mask of size w*h (255 inside,
// 0 outside). Polygon points are in SOURCE coords; (ox,oy) is the crop's origin in
// source coords so points map to crop-local pixels. A polygon with < 3 points
// yields an all-255 mask (plain rectangular crop fallback).
inline void fill_polygon(std::vector<uint8_t>& mask, int w, int h,
                         const std::vector<std::array<float, 2>>& poly,
                         float ox, float oy) {
    mask.assign(static_cast<size_t>(w) * h, 0);
    if (poly.size() < 3) {
        std::fill(mask.begin(), mask.end(), 255);
        return;
    }
    const size_t n = poly.size();
    for (int y = 0; y < h; ++y) {
        const float yc = static_cast<float>(y) + 0.5f + oy;
        std::vector<float> xs;
        for (size_t i = 0, j = n - 1; i < n; j = i++) {
            const float yi = poly[i][1], yj = poly[j][1];
            if ((yi <= yc && yj > yc) || (yj <= yc && yi > yc)) {
                const float t = (yc - yi) / (yj - yi);
                xs.push_back(poly[i][0] + t * (poly[j][0] - poly[i][0]) - ox);
            }
        }
        std::sort(xs.begin(), xs.end());
        for (size_t k = 0; k + 1 < xs.size(); k += 2) {
            int x0 = std::max(0, static_cast<int>(std::ceil(xs[k] - 0.5f)));
            int x1 = std::min(w - 1, static_cast<int>(std::floor(xs[k + 1] - 0.5f)));
            for (int x = x0; x <= x1; ++x) mask[static_cast<size_t>(y) * w + x] = 255;
        }
    }
}

// In-place 3x3 box blur (one feather pass) of an 8-bit mask.
inline void box_blur3(std::vector<uint8_t>& m, int w, int h) {
    if (w < 3 || h < 3) return;
    std::vector<uint8_t> src = m;
    for (int y = 1; y < h - 1; ++y) {
        for (int x = 1; x < w - 1; ++x) {
            int s = 0;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                    s += src[static_cast<size_t>(y + dy) * w + (x + dx)];
            m[static_cast<size_t>(y) * w + x] = static_cast<uint8_t>(s / 9);
        }
    }
}

// Premultiply a packed RGB24 buffer by an 8-bit matte (per pixel: c = c*m/255), so
// background pixels (matte 0) go black and survive lossy JPEG cleanly.
inline void premultiply_rgb(std::vector<uint8_t>& rgb, const std::vector<uint8_t>& mask) {
    const size_t n = std::min(mask.size(), rgb.size() / 3);
    for (size_t p = 0; p < n; ++p) {
        const int m = mask[p];
        uint8_t* px = rgb.data() + p * 3;
        px[0] = static_cast<uint8_t>(px[0] * m / 255);
        px[1] = static_cast<uint8_t>(px[1] * m / 255);
        px[2] = static_cast<uint8_t>(px[2] * m / 255);
    }
}

} // namespace review
} // namespace zm
