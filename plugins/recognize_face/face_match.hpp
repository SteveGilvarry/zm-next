#pragma once

// Pure, runtime-free helpers for face recognition: embedding normalization,
// cosine matching against a gallery, and a crop+bilinear-resize of an RGB24
// region. No ONNX Runtime dependency — unit-testable without a model.

#include <cstdint>
#include <string>
#include <vector>
#include <cmath>
#include <algorithm>

namespace zm::face {

// L2-normalize an embedding in place. Zero/near-zero vectors are left unchanged.
inline void l2_normalize(std::vector<float>& v) {
    double sum = 0.0;
    for (float x : v) sum += static_cast<double>(x) * x;
    const double norm = std::sqrt(sum);
    if (norm <= 1e-12) return;
    const float inv = static_cast<float>(1.0 / norm);
    for (float& x : v) x *= inv;
}

// Cosine similarity. Assumes both inputs are already L2-normalized, so this is
// just a dot product. Returns 0 if sizes mismatch or either is empty.
inline float cosine(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.empty() || a.size() != b.size()) return 0.0f;
    double dot = 0.0;
    for (size_t i = 0; i < a.size(); ++i)
        dot += static_cast<double>(a[i]) * b[i];
    return static_cast<float>(dot);
}

// A known person: a name and a (L2-normalized) reference embedding.
struct GalleryEntry {
    std::string name;
    std::vector<float> emb;
};

// The result of matching a probe embedding against a gallery.
struct Match {
    std::string name;
    float score = 0.0f;
};

// Best cosine match for `emb` over `gallery`. Returns the matched name when the
// best score is >= threshold, otherwise {"unknown", bestscore}. An empty
// gallery yields {"unknown", 0}.
inline Match best_match(const std::vector<GalleryEntry>& gallery,
                        const std::vector<float>& emb, float threshold) {
    Match m;
    m.name = "unknown";
    m.score = 0.0f;
    bool first = true;
    std::string bestName;
    for (const auto& g : gallery) {
        const float s = cosine(g.emb, emb);
        if (first || s > m.score) {
            m.score = s;
            bestName = g.name;
            first = false;
        }
    }
    if (!first && m.score >= threshold) m.name = bestName;
    return m;
}

// Crop the rectangle (x,y,w,h) from an interleaved RGB24 source image
// (sw x sh) and bilinearly resize it to (dw x dh), writing interleaved RGB24
// into `out` (resized to dw*dh*3). The crop rect is clamped to the source
// bounds. Out-of-range / empty rects produce a zero-filled buffer.
inline void crop_resize_rgb(const uint8_t* src, int sw, int sh,
                            int x, int y, int w, int h,
                            int dw, int dh, std::vector<uint8_t>& out) {
    out.assign(static_cast<size_t>(dw) * dh * 3, 0);
    if (!src || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;

    // Clamp the crop rectangle to the source image.
    int x0 = std::clamp(x, 0, sw - 1);
    int y0 = std::clamp(y, 0, sh - 1);
    int x1 = std::clamp(x + w, 0, sw);
    int y1 = std::clamp(y + h, 0, sh);
    const int cw = x1 - x0;
    const int ch = y1 - y0;
    if (cw <= 0 || ch <= 0) return;

    // Map each destination pixel back into the crop region (bilinear).
    const float sxRatio = static_cast<float>(cw) / static_cast<float>(dw);
    const float syRatio = static_cast<float>(ch) / static_cast<float>(dh);
    for (int dy = 0; dy < dh; ++dy) {
        float sy = (dy + 0.5f) * syRatio - 0.5f + static_cast<float>(y0);
        int yy0 = std::clamp(static_cast<int>(std::floor(sy)), 0, sh - 1);
        int yy1 = std::min(yy0 + 1, sh - 1);
        float wy = sy - std::floor(sy);
        wy = std::clamp(wy, 0.0f, 1.0f);
        for (int dx = 0; dx < dw; ++dx) {
            float sx = (dx + 0.5f) * sxRatio - 0.5f + static_cast<float>(x0);
            int xx0 = std::clamp(static_cast<int>(std::floor(sx)), 0, sw - 1);
            int xx1 = std::min(xx0 + 1, sw - 1);
            float wx = sx - std::floor(sx);
            wx = std::clamp(wx, 0.0f, 1.0f);
            for (int c = 0; c < 3; ++c) {
                const float p00 = src[(yy0 * sw + xx0) * 3 + c];
                const float p01 = src[(yy0 * sw + xx1) * 3 + c];
                const float p10 = src[(yy1 * sw + xx0) * 3 + c];
                const float p11 = src[(yy1 * sw + xx1) * 3 + c];
                const float top = p00 + (p01 - p00) * wx;
                const float bot = p10 + (p11 - p10) * wx;
                float val = top + (bot - top) * wy;
                val = std::clamp(val, 0.0f, 255.0f);
                out[(dy * dw + dx) * 3 + c] = static_cast<uint8_t>(val + 0.5f);
            }
        }
    }
}

} // namespace zm::face
