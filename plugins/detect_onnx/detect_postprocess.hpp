#pragma once

// Pure, runtime-free pre/post-processing for the ONNX detector. Kept separate
// from the ONNX Runtime session so it can be unit-tested without a model.

#include <cstdint>
#include <vector>
#include <algorithm>
#include <cmath>

namespace zm::detect {

// Letterbox mapping: how a (src_w x src_h) image is scaled + padded into a
// square `net`x`net` network input while preserving aspect ratio.
struct Letterbox {
    int   net   = 640;
    int   src_w = 0;
    int   src_h = 0;
    float scale = 1.0f;  // applied to source pixels to get net-space pixels
    int   pad_x = 0;     // left padding in net space
    int   pad_y = 0;     // top padding in net space
};

inline Letterbox compute_letterbox(int src_w, int src_h, int net) {
    Letterbox lb;
    lb.net = net;
    lb.src_w = src_w;
    lb.src_h = src_h;
    lb.scale = std::min(static_cast<float>(net) / static_cast<float>(src_w),
                        static_cast<float>(net) / static_cast<float>(src_h));
    const int new_w = static_cast<int>(std::round(src_w * lb.scale));
    const int new_h = static_cast<int>(std::round(src_h * lb.scale));
    lb.pad_x = (net - new_w) / 2;
    lb.pad_y = (net - new_h) / 2;
    return lb;
}

// Bilinear letterbox of an interleaved RGB24 source into a planar, normalized
// CHW float buffer (3*net*net), padding value 114/255 (YOLO convention).
// `dst` must hold 3*net*net floats.
inline void letterbox_rgb_to_chw(const uint8_t* rgb, const Letterbox& lb, float* dst) {
    const int net = lb.net;
    const float pad = 114.0f / 255.0f;
    const int plane = net * net;
    for (int i = 0; i < 3 * plane; ++i) dst[i] = pad;

    const int new_w = static_cast<int>(std::round(lb.src_w * lb.scale));
    const int new_h = static_cast<int>(std::round(lb.src_h * lb.scale));
    if (new_w <= 0 || new_h <= 0) return;

    for (int y = 0; y < new_h; ++y) {
        // Map dst row -> source row (bilinear).
        const float sy = (y + 0.5f) / lb.scale - 0.5f;
        const int y0 = std::clamp(static_cast<int>(std::floor(sy)), 0, lb.src_h - 1);
        const int y1 = std::min(y0 + 1, lb.src_h - 1);
        const float wy = sy - std::floor(sy);
        const int dy = y + lb.pad_y;
        for (int x = 0; x < new_w; ++x) {
            const float sx = (x + 0.5f) / lb.scale - 0.5f;
            const int x0 = std::clamp(static_cast<int>(std::floor(sx)), 0, lb.src_w - 1);
            const int x1 = std::min(x0 + 1, lb.src_w - 1);
            const float wx = sx - std::floor(sx);
            const int dx = x + lb.pad_x;
            for (int c = 0; c < 3; ++c) {
                const float p00 = rgb[(y0 * lb.src_w + x0) * 3 + c];
                const float p01 = rgb[(y0 * lb.src_w + x1) * 3 + c];
                const float p10 = rgb[(y1 * lb.src_w + x0) * 3 + c];
                const float p11 = rgb[(y1 * lb.src_w + x1) * 3 + c];
                const float top = p00 + (p01 - p00) * wx;
                const float bot = p10 + (p11 - p10) * wx;
                dst[c * plane + dy * net + dx] = (top + (bot - top) * wy) / 255.0f;
            }
        }
    }
}

// A detection in source-image pixel coordinates (x,y = top-left; w,h = size).
struct Box {
    float x = 0, y = 0, w = 0, h = 0;
    float confidence = 0;
    int   class_id = -1;
};

// Map a net-space xyxy box back to source-image pixels via the letterbox.
inline Box unletterbox_xyxy(float x1, float y1, float x2, float y2, const Letterbox& lb) {
    Box b;
    const float sx1 = (x1 - lb.pad_x) / lb.scale;
    const float sy1 = (y1 - lb.pad_y) / lb.scale;
    const float sx2 = (x2 - lb.pad_x) / lb.scale;
    const float sy2 = (y2 - lb.pad_y) / lb.scale;
    b.x = std::clamp(sx1, 0.0f, static_cast<float>(lb.src_w));
    b.y = std::clamp(sy1, 0.0f, static_cast<float>(lb.src_h));
    b.w = std::clamp(sx2, 0.0f, static_cast<float>(lb.src_w)) - b.x;
    b.h = std::clamp(sy2, 0.0f, static_cast<float>(lb.src_h)) - b.y;
    return b;
}

// Decode a YOLO26-style NMS-free output tensor [num x 6] = (x1,y1,x2,y2,conf,
// class_id), already de-duplicated, into source-pixel boxes. Only a confidence
// threshold (and optional class allow-list) is applied.
inline std::vector<Box> decode_nms_free(const float* out, int num, const Letterbox& lb,
                                        float conf_thr,
                                        const std::vector<int>& allow = {}) {
    std::vector<Box> boxes;
    for (int i = 0; i < num; ++i) {
        const float* r = out + i * 6;
        const float conf = r[4];
        if (conf < conf_thr) continue;
        const int cls = static_cast<int>(std::lround(r[5]));
        if (!allow.empty() && std::find(allow.begin(), allow.end(), cls) == allow.end())
            continue;
        Box b = unletterbox_xyxy(r[0], r[1], r[2], r[3], lb);
        if (b.w <= 0 || b.h <= 0) continue;
        b.confidence = conf;
        b.class_id = cls;
        boxes.push_back(b);
    }
    return boxes;
}

// Greedy IoU de-duplication (keep highest-confidence box per cluster, same class).
// Used to merge detections from overlapping ROI crops and the full-frame sweep.
inline std::vector<Box> merge_overlapping(std::vector<Box> boxes, float iou_thr = 0.5f) {
    std::sort(boxes.begin(), boxes.end(),
              [](const Box& a, const Box& b) { return a.confidence > b.confidence; });
    std::vector<Box> keep;
    for (const auto& b : boxes) {
        bool dup = false;
        for (const auto& k : keep) {
            if (k.class_id != b.class_id) continue;
            const float ix = std::max(0.f, std::min(b.x + b.w, k.x + k.w) - std::max(b.x, k.x));
            const float iy = std::max(0.f, std::min(b.y + b.h, k.y + k.h) - std::max(b.y, k.y));
            const float inter = ix * iy, uni = b.w * b.h + k.w * k.h - inter;
            if (uni > 0 && inter / uni > iou_thr) { dup = true; break; }
        }
        if (!dup) keep.push_back(b);
    }
    return keep;
}

} // namespace zm::detect
