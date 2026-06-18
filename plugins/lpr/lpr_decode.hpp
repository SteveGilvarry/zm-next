#pragma once

// Pure, runtime-free helpers for the LPR (ALPR) plugin. Kept separate from the
// ONNX Runtime session so they can be unit-tested without a model. No ORT
// dependency here — only the C++ standard library.

#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>
#include <cctype>

namespace zm::lpr {

// Normalize a plate string for comparison/watchlist matching: uppercase and
// strip everything that is not an ASCII letter or digit (spaces, hyphens, etc.).
inline std::string normalize_plate(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (unsigned char c : in) {
        if (std::isalnum(c)) {
            out.push_back(static_cast<char>(std::toupper(c)));
        }
    }
    return out;
}

// Greedy CTC decode of a [T, C] logit (or probability) matrix laid out row-major
// (timestep-major): logits[t*C + c]. For each timestep take the argmax class;
// then collapse consecutive duplicate indices and drop the blank index. The
// resulting class indices are mapped through `charset` (index k -> charset[k])
// when k < charset.size(); indices outside the charset range are skipped.
//
// `blank` selects the CTC blank index; pass C-1 for the common "last index is
// blank" convention.
inline std::string ctc_greedy_decode(const float* logits, int T, int C,
                                     const std::string& charset, int blank) {
    std::string out;
    if (!logits || T <= 0 || C <= 0) return out;
    int prev = -1;
    for (int t = 0; t < T; ++t) {
        const float* row = logits + static_cast<size_t>(t) * C;
        int best = 0;
        float bestVal = row[0];
        for (int c = 1; c < C; ++c) {
            if (row[c] > bestVal) {
                bestVal = row[c];
                best = c;
            }
        }
        // Collapse repeats and drop blanks.
        if (best != prev) {
            if (best != blank && best >= 0 &&
                best < static_cast<int>(charset.size())) {
                out.push_back(charset[best]);
            }
            prev = best;
        }
    }
    return out;
}

// Approximate per-sequence confidence: mean over timesteps of the max softmax
// probability. This is an APPROXIMATION — it averages over every timestep
// (including blank/repeat timesteps), not just the emitted characters, so it is
// a coarse quality signal rather than a calibrated per-character confidence.
inline float ctc_mean_confidence(const float* logits, int T, int C) {
    if (!logits || T <= 0 || C <= 0) return 0.0f;
    double sum = 0.0;
    for (int t = 0; t < T; ++t) {
        const float* row = logits + static_cast<size_t>(t) * C;
        // Softmax over the row, take the max probability.
        float maxLogit = row[0];
        for (int c = 1; c < C; ++c) maxLogit = std::max(maxLogit, row[c]);
        double denom = 0.0;
        for (int c = 0; c < C; ++c) denom += std::exp(static_cast<double>(row[c]) - maxLogit);
        // max prob corresponds to the max logit.
        double maxProb = denom > 0.0 ? (std::exp(0.0) / denom) : 0.0;
        sum += maxProb;
    }
    return static_cast<float>(sum / T);
}

// True if `plate` (normalized) matches any entry of `list` (each normalized).
inline bool watchlisted(const std::vector<std::string>& list, const std::string& plate) {
    const std::string p = normalize_plate(plate);
    if (p.empty()) return false;
    for (const auto& w : list) {
        if (normalize_plate(w) == p) return true;
    }
    return false;
}

// Crop a rectangular region [sx,sy,sw,sh] (source pixels) from an interleaved
// RGB24 source image (src_w x src_h) and bilinearly resize it into a normalized
// CHW float buffer of size 3*dst_h*dst_w (planar R,G,B), values /255. `dst` must
// hold 3*dst_h*dst_w floats. The crop is clamped to the source bounds.
inline void crop_resize_rgb(const uint8_t* src, int src_w, int src_h,
                            float sx, float sy, float sw, float sh,
                            int dst_w, int dst_h, float* dst) {
    if (!src || !dst || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) return;
    // Clamp crop rectangle to source bounds.
    float x0 = std::clamp(sx, 0.0f, static_cast<float>(src_w));
    float y0 = std::clamp(sy, 0.0f, static_cast<float>(src_h));
    float x1 = std::clamp(sx + sw, 0.0f, static_cast<float>(src_w));
    float y1 = std::clamp(sy + sh, 0.0f, static_cast<float>(src_h));
    float cw = std::max(1.0f, x1 - x0);
    float ch = std::max(1.0f, y1 - y0);

    const int plane = dst_w * dst_h;
    for (int dy = 0; dy < dst_h; ++dy) {
        // Map dst row -> source y inside the crop.
        const float fy = (dy + 0.5f) / dst_h * ch + y0 - 0.5f;
        const int iy0 = std::clamp(static_cast<int>(std::floor(fy)), 0, src_h - 1);
        const int iy1 = std::min(iy0 + 1, src_h - 1);
        const float wy = fy - std::floor(fy);
        for (int dx = 0; dx < dst_w; ++dx) {
            const float fx = (dx + 0.5f) / dst_w * cw + x0 - 0.5f;
            const int ix0 = std::clamp(static_cast<int>(std::floor(fx)), 0, src_w - 1);
            const int ix1 = std::min(ix0 + 1, src_w - 1);
            const float wx = fx - std::floor(fx);
            for (int c = 0; c < 3; ++c) {
                const float p00 = src[(iy0 * src_w + ix0) * 3 + c];
                const float p01 = src[(iy0 * src_w + ix1) * 3 + c];
                const float p10 = src[(iy1 * src_w + ix0) * 3 + c];
                const float p11 = src[(iy1 * src_w + ix1) * 3 + c];
                const float top = p00 + (p01 - p00) * wx;
                const float bot = p10 + (p11 - p10) * wx;
                dst[c * plane + dy * dst_w + dx] = (top + (bot - top) * wy) / 255.0f;
            }
        }
    }
}

// Grayscale variant of crop_resize_rgb: output is a single-channel CHW buffer of
// size dst_h*dst_w (i.e. [1, dst_h, dst_w]), normalized /255. Grayscale is the
// luminance of the bilinearly-sampled RGB pixel (Rec.601 weights).
inline void crop_resize_gray(const uint8_t* src, int src_w, int src_h,
                             float sx, float sy, float sw, float sh,
                             int dst_w, int dst_h, float* dst) {
    if (!src || !dst || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) return;
    float x0 = std::clamp(sx, 0.0f, static_cast<float>(src_w));
    float y0 = std::clamp(sy, 0.0f, static_cast<float>(src_h));
    float x1 = std::clamp(sx + sw, 0.0f, static_cast<float>(src_w));
    float y1 = std::clamp(sy + sh, 0.0f, static_cast<float>(src_h));
    float cw = std::max(1.0f, x1 - x0);
    float ch = std::max(1.0f, y1 - y0);

    for (int dy = 0; dy < dst_h; ++dy) {
        const float fy = (dy + 0.5f) / dst_h * ch + y0 - 0.5f;
        const int iy0 = std::clamp(static_cast<int>(std::floor(fy)), 0, src_h - 1);
        const int iy1 = std::min(iy0 + 1, src_h - 1);
        const float wy = fy - std::floor(fy);
        for (int dx = 0; dx < dst_w; ++dx) {
            const float fx = (dx + 0.5f) / dst_w * cw + x0 - 0.5f;
            const int ix0 = std::clamp(static_cast<int>(std::floor(fx)), 0, src_w - 1);
            const int ix1 = std::min(ix0 + 1, src_w - 1);
            const float wx = fx - std::floor(fx);
            float gray = 0.0f;
            // Rec.601 luma weights.
            static const float kW[3] = {0.299f, 0.587f, 0.114f};
            for (int c = 0; c < 3; ++c) {
                const float p00 = src[(iy0 * src_w + ix0) * 3 + c];
                const float p01 = src[(iy0 * src_w + ix1) * 3 + c];
                const float p10 = src[(iy1 * src_w + ix0) * 3 + c];
                const float p11 = src[(iy1 * src_w + ix1) * 3 + c];
                const float top = p00 + (p01 - p00) * wx;
                const float bot = p10 + (p11 - p10) * wx;
                gray += kW[c] * (top + (bot - top) * wy);
            }
            dst[dy * dst_w + dx] = gray / 255.0f;
        }
    }
}

} // namespace zm::lpr
