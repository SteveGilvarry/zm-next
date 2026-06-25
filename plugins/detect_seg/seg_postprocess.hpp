#pragma once

// Pure, runtime-free post-processing for the YOLO-seg instance segmentation
// detector. Kept separate from the ONNX Runtime session so it can be
// unit-tested without a model.
//
// Reuses Letterbox / unletterbox math from the detect_onnx shared header.

#include "../detect_onnx/detect_postprocess.hpp"

#include <cstdint>
#include <vector>
#include <array>
#include <algorithm>
#include <cmath>

namespace zm::seg {

// A segmented object in source-image pixel coordinates (x,y = top-left;
// w,h = size). Carries its raw mask coefficients (so NMS can run on boxes
// first, before the relatively expensive coeff*proto mask synthesis) and,
// after mask building, a coarse outer-contour polygon (source pixels).
struct SegObj {
    float x = 0, y = 0, w = 0, h = 0;
    float confidence = 0;
    int   class_id = -1;
    std::vector<float> coeffs;                  // nm mask coefficients
    std::vector<std::array<float, 2>> polygon;  // [x,y] source pixels
};

// Intersection-over-union of two object boxes (xywh, top-left origin).
inline float iou(const SegObj& a, const SegObj& b) {
    const float ax2 = a.x + a.w, ay2 = a.y + a.h;
    const float bx2 = b.x + b.w, by2 = b.y + b.h;
    const float ix1 = std::max(a.x, b.x);
    const float iy1 = std::max(a.y, b.y);
    const float ix2 = std::min(ax2, bx2);
    const float iy2 = std::min(ay2, by2);
    const float iw = std::max(0.0f, ix2 - ix1);
    const float ih = std::max(0.0f, iy2 - iy1);
    const float inter = iw * ih;
    const float areaA = std::max(0.0f, a.w) * std::max(0.0f, a.h);
    const float areaB = std::max(0.0f, b.w) * std::max(0.0f, b.h);
    const float uni = areaA + areaB - inter;
    if (uni <= 0.0f) return 0.0f;
    return inter / uni;
}

// Greedy IoU non-maximum suppression. Returns the kept objects (descending
// confidence), carrying their mask coefficients through. Class-aware: only
// objects of the same class suppress one another.
inline std::vector<SegObj> nms(std::vector<SegObj> objs, float iou_thr) {
    std::sort(objs.begin(), objs.end(),
              [](const SegObj& a, const SegObj& b) { return a.confidence > b.confidence; });
    std::vector<SegObj> kept;
    std::vector<char> suppressed(objs.size(), 0);
    for (size_t i = 0; i < objs.size(); ++i) {
        if (suppressed[i]) continue;
        kept.push_back(objs[i]);
        for (size_t j = i + 1; j < objs.size(); ++j) {
            if (suppressed[j]) continue;
            if (objs[i].class_id != objs[j].class_id) continue;
            if (iou(objs[i], objs[j]) > iou_thr) suppressed[j] = 1;
        }
    }
    return kept;
}

// Decode the YOLO-seg detection output (output0) into pre-NMS objects in
// source pixels, carrying the mask coefficients.
//
// `out` points at `num` candidates. The detection tensor has `channels` rows
// laid out as [4 box (cx,cy,w,h)] + [nc class scores] + [nm mask coeffs].
// `channel_major` selects the memory layout:
//   channel-major ([1,channels,num]): value of channel c for candidate i is
//   out[c*num + i].
//   candidate-major ([1,num,channels]): out[i*channels + c].
// Class = argmax over the nc class scores; conf = that max. Boxes are mapped
// back to source pixels via the letterbox. Only candidates with conf >=
// conf_thr (and, if `allow` is non-empty, an allowed class) are kept.
inline std::vector<SegObj> decode(const float* out, int num, int channels,
                                  int num_classes, int mask_dim,
                                  bool channel_major, const zm::detect::Letterbox& lb,
                                  float conf_thr, const std::vector<int>& allow = {}) {
    std::vector<SegObj> objs;
    auto at = [&](int i, int c) -> float {
        return channel_major ? out[c * num + i] : out[i * channels + c];
    };

    for (int i = 0; i < num; ++i) {
        // argmax over the class scores.
        int best = -1;
        float bestScore = -1.0f;
        for (int c = 0; c < num_classes; ++c) {
            const float s = at(i, 4 + c);
            if (s > bestScore) { bestScore = s; best = c; }
        }
        if (bestScore < conf_thr) continue;
        if (!allow.empty() && std::find(allow.begin(), allow.end(), best) == allow.end())
            continue;

        const float cx = at(i, 0);
        const float cy = at(i, 1);
        const float w = at(i, 2);
        const float h = at(i, 3);

        zm::detect::Box b = zm::detect::unletterbox_xyxy(
            cx - w * 0.5f, cy - h * 0.5f, cx + w * 0.5f, cy + h * 0.5f, lb);
        if (b.w <= 0 || b.h <= 0) continue;

        SegObj o;
        o.x = b.x; o.y = b.y; o.w = b.w; o.h = b.h;
        o.confidence = bestScore;
        o.class_id = best;
        o.coeffs.reserve(mask_dim);
        const int coeffBase = 4 + num_classes;
        for (int k = 0; k < mask_dim; ++k)
            o.coeffs.push_back(at(i, coeffBase + k));
        objs.push_back(std::move(o));
    }
    return objs;
}

inline float sigmoid(float v) { return 1.0f / (1.0f + std::exp(-v)); }

// Combine a single object's mask coefficients with the prototype masks to
// synthesize an mh*mw float mask in [0,1] (sigmoid of the linear combination).
//
// `proto` is the proto tensor (output1) data of shape [nm, mh, mw], i.e.
// proto[k*mh*mw + y*mw + x]. `coeffs` has `mask_dim` entries. The result is
// row-major mh*mw. No thresholding is applied here.
inline std::vector<float> build_mask(const float* proto, int mask_dim,
                                     int mh, int mw, const std::vector<float>& coeffs) {
    std::vector<float> mask(static_cast<size_t>(mh) * mw, 0.0f);
    const int plane = mh * mw;
    const int km = std::min(mask_dim, static_cast<int>(coeffs.size()));
    for (int k = 0; k < km; ++k) {
        const float ck = coeffs[k];
        const float* pp = proto + static_cast<size_t>(k) * plane;
        for (int p = 0; p < plane; ++p) mask[p] += ck * pp[p];
    }
    for (int p = 0; p < plane; ++p) mask[p] = sigmoid(mask[p]);
    return mask;
}

// Convert a thresholded mask (in mask-grid coords mh*mw) to a coarse outer
// polygon in SOURCE pixel coordinates.
//
// Method (documented "coarse but valid" approach): scan each mask row and find
// the min/max x of set pixels (mask >= thr) that also fall inside the
// detection box's mask-grid footprint. Each occupied row contributes two
// boundary points (left edge, right edge). We walk down the left edges then
// back up the right edges, producing a single closed polygon ring that hugs
// the mask's horizontal extent per row. Rows are subsampled by `row_step` to
// keep the polygon compact.
//
// Coordinate mapping: the mask grid corresponds to the letterboxed `net`x`net`
// input scaled down by net/mask_w. So a mask pixel (mx,my) maps to net space
// as (mx * net/mw, my * net/mh), then unletterbox (subtract pad, divide scale)
// to source pixels. We pass the Letterbox and mask dims to do this.
//
// `box` (source pixels, xywh) clips the mask so only pixels inside the
// detection box contribute (YOLO-seg crops the mask to the box).
// A downscaled soft-alpha cutout of one object's mask, aligned to its bbox.
// `w`*`h` row-major 8-bit alpha (0=background, 255=object); stretch it across the
// object's bbox at render time. This preserves the per-pixel soft matte the
// polygon throws away, while staying small (capped by max_edge).
struct AlphaMask {
    int w = 0, h = 0;
    std::vector<uint8_t> data;  // row-major w*h, 8-bit
};

// Crop the soft mask (mh*mw, [0,1]) to the object's bbox footprint in mask-grid
// coords, convert to 8-bit alpha, and downscale (nearest) so the longer edge is
// <= max_edge. The result maps linearly onto the object's source-pixel bbox.
inline AlphaMask mask_to_alpha(const std::vector<float>& mask, int mh, int mw,
                               const zm::detect::Letterbox& lb, const SegObj& box,
                               int max_edge = 64) {
    AlphaMask out;
    const float net = static_cast<float>(lb.net);
    const float sx = net / static_cast<float>(mw);
    const float sy = net / static_cast<float>(mh);
    auto src_to_mask_x = [&](float xs) { return (xs * lb.scale + lb.pad_x) / sx; };
    auto src_to_mask_y = [&](float ys) { return (ys * lb.scale + lb.pad_y) / sy; };
    const int bx0 = std::max(0, static_cast<int>(std::floor(src_to_mask_x(box.x))));
    const int bx1 = std::min(mw - 1, static_cast<int>(std::ceil(src_to_mask_x(box.x + box.w))));
    const int by0 = std::max(0, static_cast<int>(std::floor(src_to_mask_y(box.y))));
    const int by1 = std::min(mh - 1, static_cast<int>(std::ceil(src_to_mask_y(box.y + box.h))));
    const int cw = bx1 - bx0 + 1;
    const int ch = by1 - by0 + 1;
    if (cw < 1 || ch < 1) return out;

    // Target dims: downscale (nearest) so the long edge <= max_edge.
    int dw = cw, dh = ch;
    const int longEdge = std::max(cw, ch);
    if (max_edge > 0 && longEdge > max_edge) {
        const double s = static_cast<double>(max_edge) / longEdge;
        dw = std::max(1, static_cast<int>(cw * s));
        dh = std::max(1, static_cast<int>(ch * s));
    }
    out.w = dw; out.h = dh;
    out.data.resize(static_cast<size_t>(dw) * dh);
    for (int y = 0; y < dh; ++y) {
        const int my = by0 + std::min(ch - 1, y * ch / dh);
        for (int x = 0; x < dw; ++x) {
            const int mx = bx0 + std::min(cw - 1, x * cw / dw);
            float v = mask[static_cast<size_t>(my) * mw + mx];
            if (v < 0.0f) v = 0.0f; else if (v > 1.0f) v = 1.0f;
            out.data[static_cast<size_t>(y) * dw + x] = static_cast<uint8_t>(v * 255.0f + 0.5f);
        }
    }
    return out;
}

inline std::vector<std::array<float, 2>> mask_to_polygon(
        const std::vector<float>& mask, int mh, int mw, float thr,
        const zm::detect::Letterbox& lb, const SegObj& box, int row_step = 2) {
    if (row_step < 1) row_step = 1;
    const float net = static_cast<float>(lb.net);
    const float sx = net / static_cast<float>(mw);  // mask px -> net px (x)
    const float sy = net / static_cast<float>(mh);  // mask px -> net px (y)

    // Box footprint in mask-grid coords (source -> net -> mask).
    auto src_to_mask_x = [&](float xs) {
        return (xs * lb.scale + lb.pad_x) / sx;
    };
    auto src_to_mask_y = [&](float ys) {
        return (ys * lb.scale + lb.pad_y) / sy;
    };
    const int bx0 = std::max(0, static_cast<int>(std::floor(src_to_mask_x(box.x))));
    const int bx1 = std::min(mw - 1, static_cast<int>(std::ceil(src_to_mask_x(box.x + box.w))));
    const int by0 = std::max(0, static_cast<int>(std::floor(src_to_mask_y(box.y))));
    const int by1 = std::min(mh - 1, static_cast<int>(std::ceil(src_to_mask_y(box.y + box.h))));

    // Map a mask pixel center to source pixels.
    auto mask_to_src = [&](float mx, float my) -> std::array<float, 2> {
        const float nx = mx * sx;
        const float ny = my * sy;
        float xs = (nx - lb.pad_x) / lb.scale;
        float ys = (ny - lb.pad_y) / lb.scale;
        xs = std::clamp(xs, 0.0f, static_cast<float>(lb.src_w));
        ys = std::clamp(ys, 0.0f, static_cast<float>(lb.src_h));
        return {xs, ys};
    };

    std::vector<std::array<int, 2>> leftEdges;   // (x_min, y) per occupied row
    std::vector<std::array<int, 2>> rightEdges;  // (x_max, y) per occupied row
    for (int y = by0; y <= by1; y += row_step) {
        int xmin = -1, xmax = -1;
        for (int x = bx0; x <= bx1; ++x) {
            if (mask[static_cast<size_t>(y) * mw + x] >= thr) {
                if (xmin < 0) xmin = x;
                xmax = x;
            }
        }
        if (xmin >= 0) {
            leftEdges.push_back({xmin, y});
            rightEdges.push_back({xmax, y});
        }
    }

    std::vector<std::array<float, 2>> poly;
    if (leftEdges.empty()) return poly;

    poly.reserve(leftEdges.size() * 2);
    // Down the left edges (top -> bottom).
    for (const auto& e : leftEdges)
        poly.push_back(mask_to_src(static_cast<float>(e[0]), static_cast<float>(e[1])));
    // Up the right edges (bottom -> top).
    for (auto it = rightEdges.rbegin(); it != rightEdges.rend(); ++it)
        poly.push_back(mask_to_src(static_cast<float>((*it)[0] + 1),
                                   static_cast<float>((*it)[1])));
    return poly;
}

} // namespace zm::seg
