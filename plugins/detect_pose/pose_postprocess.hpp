#pragma once

// Pure, runtime-free post-processing for the YOLO-pose detector. Kept separate
// from the ONNX Runtime session so it can be unit-tested without a model.
//
// Reuses Letterbox / unletterbox math from the detect_onnx shared header.

#include "../detect_onnx/detect_postprocess.hpp"

#include <cstdint>
#include <vector>
#include <algorithm>
#include <cmath>

namespace zm::pose {

// One COCO keypoint in source-image pixel coordinates with a visibility score.
struct Keypoint {
    float x = 0, y = 0, v = 0;
};

// A detected person in source-image pixels (x,y = top-left; w,h = size) with its
// 17 keypoints carried through.
struct Person {
    float x = 0, y = 0, w = 0, h = 0;
    float confidence = 0;
    std::vector<Keypoint> kpts;
};

// Intersection-over-union of two person boxes (xywh, top-left origin).
inline float iou(const Person& a, const Person& b) {
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

// Greedy IoU non-maximum suppression. Returns the kept persons (descending
// confidence), carrying their keypoints through.
inline std::vector<Person> nms(std::vector<Person> persons, float iou_thr) {
    std::sort(persons.begin(), persons.end(),
              [](const Person& a, const Person& b) { return a.confidence > b.confidence; });
    std::vector<Person> kept;
    std::vector<char> suppressed(persons.size(), 0);
    for (size_t i = 0; i < persons.size(); ++i) {
        if (suppressed[i]) continue;
        kept.push_back(persons[i]);
        for (size_t j = i + 1; j < persons.size(); ++j) {
            if (suppressed[j]) continue;
            if (iou(persons[i], persons[j]) > iou_thr) suppressed[j] = 1;
        }
    }
    return kept;
}

// Decode a YOLO-pose output tensor into pre-NMS persons in source pixels.
//
// `out` points at `num` candidates, each `stride` floats apart. Each candidate
// is laid out as [cx, cy, w, h, conf, then num_kpts*(kx,ky,vis)] in net pixels.
// This caller-supplied layout lets the same function handle both the
// [1,56,8400] (channel-major) and [1,8400,56] (candidate-major) ONNX exports:
//
//   candidate-major ([1,8400,56]): pass stride = 56, and `at(i, c)` reads
//   out[i*stride + c].
//   channel-major  ([1,56,8400]): the values for channel c of candidate i live
//   at out[c*num + i]; the caller transposes via the `channel_major` flag.
//
// `num_kpts` is normally 17 (COCO). Boxes/keypoints are mapped back to source
// pixels via the letterbox. Only candidates with conf >= conf_thr are kept.
inline std::vector<Person> decode(const float* out, int num, int num_kpts,
                                  bool channel_major,
                                  const zm::detect::Letterbox& lb, float conf_thr) {
    std::vector<Person> persons;
    const int values = 5 + num_kpts * 3;  // 56 for 17 keypoints

    // Reader that hides the channel/candidate-major distinction.
    auto at = [&](int i, int c) -> float {
        return channel_major ? out[c * num + i] : out[i * values + c];
    };

    for (int i = 0; i < num; ++i) {
        const float conf = at(i, 4);
        if (conf < conf_thr) continue;

        const float cx = at(i, 0);
        const float cy = at(i, 1);
        const float w = at(i, 2);
        const float h = at(i, 3);

        // Center form -> net-space xyxy, then unletterbox to source pixels.
        zm::detect::Box b = zm::detect::unletterbox_xyxy(
            cx - w * 0.5f, cy - h * 0.5f, cx + w * 0.5f, cy + h * 0.5f, lb);
        if (b.w <= 0 || b.h <= 0) continue;

        Person p;
        p.x = b.x;
        p.y = b.y;
        p.w = b.w;
        p.h = b.h;
        p.confidence = conf;
        p.kpts.reserve(num_kpts);
        for (int k = 0; k < num_kpts; ++k) {
            const float kx = at(i, 5 + k * 3 + 0);
            const float ky = at(i, 5 + k * 3 + 1);
            const float kv = at(i, 5 + k * 3 + 2);
            Keypoint kp;
            // Keypoints are net pixels: undo letterbox scale + pad (no clamp to
            // the box, but clamp to the source frame).
            kp.x = (kx - lb.pad_x) / lb.scale;
            kp.y = (ky - lb.pad_y) / lb.scale;
            kp.x = std::clamp(kp.x, 0.0f, static_cast<float>(lb.src_w));
            kp.y = std::clamp(kp.y, 0.0f, static_cast<float>(lb.src_h));
            kp.v = kv;
            p.kpts.push_back(kp);
        }
        persons.push_back(std::move(p));
    }
    return persons;
}

} // namespace zm::pose
