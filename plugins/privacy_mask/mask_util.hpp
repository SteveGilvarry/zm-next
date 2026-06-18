// Pure geometry/pixel helpers for the privacy_mask plugin.
//
// No ABI / plugin dependencies here so these can be unit-tested in isolation.
// All functions operate on an interleaved pixel buffer of `channels` bytes per
// pixel (1 for grayscale, 3 for RGB24), row-major, tightly packed (stride =
// w*channels). Polygon vertices are in source pixel coordinates.

#pragma once

#include <cstdint>
#include <vector>
#include <algorithm>
#include <cmath>

namespace zm {
namespace privacy {

struct Pt {
    float x;
    float y;
};

// Axis-aligned bounding box of a polygon, clamped to [0,w)x[0,h).
struct BBox {
    int x0, y0, x1, y1;  // inclusive-exclusive: [x0,x1) x [y0,y1)
    bool valid() const { return x1 > x0 && y1 > y0; }
};

inline BBox polygon_bbox(const std::vector<Pt>& poly, int w, int h) {
    BBox b{0, 0, 0, 0};
    if (poly.empty() || w <= 0 || h <= 0) return b;
    float minx = poly[0].x, maxx = poly[0].x;
    float miny = poly[0].y, maxy = poly[0].y;
    for (const auto& p : poly) {
        minx = std::min(minx, p.x);
        maxx = std::max(maxx, p.x);
        miny = std::min(miny, p.y);
        maxy = std::max(maxy, p.y);
    }
    int x0 = static_cast<int>(std::floor(minx));
    int y0 = static_cast<int>(std::floor(miny));
    int x1 = static_cast<int>(std::ceil(maxx)) + 1;  // exclusive
    int y1 = static_cast<int>(std::ceil(maxy)) + 1;
    b.x0 = std::max(0, x0);
    b.y0 = std::max(0, y0);
    b.x1 = std::min(w, x1);
    b.y1 = std::min(h, y1);
    return b;
}

// Ray-casting (even-odd) point-in-polygon test. Tests the point (x,y).
// Returns true if inside. A polygon with fewer than 3 vertices is "empty"
// (returns false). The polygon is treated as implicitly closed.
inline bool point_in_polygon(const std::vector<Pt>& poly, float x, float y) {
    const size_t n = poly.size();
    if (n < 3) return false;
    bool inside = false;
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        const float xi = poly[i].x, yi = poly[i].y;
        const float xj = poly[j].x, yj = poly[j].y;
        const bool crosses = ((yi > y) != (yj > y));
        if (crosses) {
            const float t = (y - yi) / (yj - yi);
            const float xcross = xi + t * (xj - xi);
            if (x < xcross) inside = !inside;
        }
    }
    return inside;
}

// Set every pixel whose center lies inside `poly` to 0 (black). Pixels outside
// the polygon are left untouched. Operates only within the polygon bbox.
inline void black_region(uint8_t* px, int w, int h, int channels,
                         const std::vector<Pt>& poly) {
    if (!px || w <= 0 || h <= 0 || channels <= 0) return;
    const BBox b = polygon_bbox(poly, w, h);
    if (!b.valid()) return;
    for (int y = b.y0; y < b.y1; ++y) {
        for (int x = b.x0; x < b.x1; ++x) {
            if (point_in_polygon(poly, static_cast<float>(x) + 0.5f,
                                 static_cast<float>(y) + 0.5f)) {
                uint8_t* p = px + (static_cast<size_t>(y) * w + x) * channels;
                for (int c = 0; c < channels; ++c) p[c] = 0;
            }
        }
    }
}

// Pixelate (mosaic) the polygon region: divide the bbox into blocks of
// `block`x`block`, compute the average per channel over the pixels in each
// block that lie inside the polygon, and write that average back to those
// inside-polygon pixels. Pixels outside the polygon are untouched.
inline void pixelate_region(uint8_t* px, int w, int h, int channels,
                            const std::vector<Pt>& poly, int block) {
    if (!px || w <= 0 || h <= 0 || channels <= 0) return;
    if (block < 1) block = 1;
    const BBox b = polygon_bbox(poly, w, h);
    if (!b.valid()) return;

    std::vector<long> sum(channels, 0);
    for (int by = b.y0; by < b.y1; by += block) {
        for (int bx = b.x0; bx < b.x1; bx += block) {
            const int ex = std::min(bx + block, b.x1);
            const int ey = std::min(by + block, b.y1);
            std::fill(sum.begin(), sum.end(), 0L);
            long count = 0;
            // First pass: accumulate over inside-polygon pixels in the block.
            for (int y = by; y < ey; ++y) {
                for (int x = bx; x < ex; ++x) {
                    if (!point_in_polygon(poly, static_cast<float>(x) + 0.5f,
                                          static_cast<float>(y) + 0.5f))
                        continue;
                    const uint8_t* p = px + (static_cast<size_t>(y) * w + x) * channels;
                    for (int c = 0; c < channels; ++c) sum[c] += p[c];
                    ++count;
                }
            }
            if (count == 0) continue;
            // Second pass: write the average to inside-polygon pixels.
            uint8_t avg[4] = {0, 0, 0, 0};
            for (int c = 0; c < channels && c < 4; ++c)
                avg[c] = static_cast<uint8_t>(sum[c] / count);
            for (int y = by; y < ey; ++y) {
                for (int x = bx; x < ex; ++x) {
                    if (!point_in_polygon(poly, static_cast<float>(x) + 0.5f,
                                          static_cast<float>(y) + 0.5f))
                        continue;
                    uint8_t* p = px + (static_cast<size_t>(y) * w + x) * channels;
                    for (int c = 0; c < channels; ++c) p[c] = avg[c];
                }
            }
        }
    }
}

// Separable box blur applied only to inside-polygon pixels, over the polygon
// bbox. `radius` is half the kernel extent (kernel size ~= 2*radius+1). Reads
// from a snapshot of the bbox so the blur is not contaminated by partial writes.
// Only inside-polygon pixels are modified.
inline void blur_region(uint8_t* px, int w, int h, int channels,
                        const std::vector<Pt>& poly, int radius) {
    if (!px || w <= 0 || h <= 0 || channels <= 0) return;
    if (radius < 1) radius = 1;
    const BBox b = polygon_bbox(poly, w, h);
    if (!b.valid()) return;
    const int bw = b.x1 - b.x0;
    const int bh = b.y1 - b.y0;

    // Snapshot the bbox region.
    std::vector<uint8_t> src(static_cast<size_t>(bw) * bh * channels);
    for (int y = 0; y < bh; ++y) {
        const uint8_t* s = px + (static_cast<size_t>(b.y0 + y) * w + b.x0) * channels;
        std::copy(s, s + static_cast<size_t>(bw) * channels,
                  src.data() + static_cast<size_t>(y) * bw * channels);
    }

    auto at = [&](int x, int y, int c) -> int {
        x = std::min(std::max(x, 0), bw - 1);
        y = std::min(std::max(y, 0), bh - 1);
        return src[(static_cast<size_t>(y) * bw + x) * channels + c];
    };

    for (int y = 0; y < bh; ++y) {
        for (int x = 0; x < bw; ++x) {
            const int gx = b.x0 + x, gy = b.y0 + y;
            if (!point_in_polygon(poly, static_cast<float>(gx) + 0.5f,
                                  static_cast<float>(gy) + 0.5f))
                continue;
            uint8_t* p = px + (static_cast<size_t>(gy) * w + gx) * channels;
            for (int c = 0; c < channels; ++c) {
                long acc = 0;
                int n = 0;
                for (int dy = -radius; dy <= radius; ++dy) {
                    for (int dx = -radius; dx <= radius; ++dx) {
                        acc += at(x + dx, y + dy, c);
                        ++n;
                    }
                }
                p[c] = static_cast<uint8_t>(acc / n);
            }
        }
    }
}

}  // namespace privacy
}  // namespace zm
