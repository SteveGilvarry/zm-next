// Dynamic (subject-following) masking for the privacy_mask plugin.
//
// Static regions cover fixed areas. Dynamic masking covers whatever the
// detectors upstream found on this frame: people, faces, number plates. The
// plugin subscribes to "detection", "tracked_detection", "face" and "lpr"
// events, stores their boxes per stream keyed by pts, and when a frame arrives
// masks every box whose pts is within `hold` of the frame's pts.
//
// Why this lines up frame-for-frame: detect plugins publish their event before
// forwarding the frame, and the host bus calls subscribers inline on the
// publisher's thread. A privacy_mask placed DOWNSTREAM of the detector therefore
// already holds frame N's boxes when frame N reaches it. `hold` covers frames
// the detector skipped (motion gate, missed detection) by reusing boxes from
// neighbouring frames; `padding` covers movement between them.
//
// Pure header, no ABI; tests/test_dynamic_mask.cpp exercises it.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace zm {
namespace privacy {

// Pixel rectangle, [x0,x1) x [y0,y1).
struct Rect {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool valid() const { return x1 > x0 && y1 > y0; }
};

// A box from an event, in source pixels (x, y, w, h).
struct Box {
    float x = 0, y = 0, w = 0, h = 0;
};

// Grow a box by `padding` of its size on every side and clamp to the frame.
inline Rect expand_clamp(const Box& b, float padding, int fw, int fh) {
    Rect r;
    if (b.w <= 0 || b.h <= 0 || fw <= 0 || fh <= 0) return r;
    const float px = b.w * padding, py = b.h * padding;
    r.x0 = std::max(0, static_cast<int>(std::floor(b.x - px)));
    r.y0 = std::max(0, static_cast<int>(std::floor(b.y - py)));
    r.x1 = std::min(fw, static_cast<int>(std::ceil(b.x + b.w + px)));
    r.y1 = std::min(fh, static_cast<int>(std::ceil(b.y + b.h + py)));
    return r;
}

// Top part of a person box, as a cheap head/face region when no face detector
// is running. `fraction` of the box height from the top.
inline Box head_of(const Box& person, float fraction) {
    Box b = person;
    b.h = person.h * std::clamp(fraction, 0.05f, 1.0f);
    return b;
}

// ---------------------------------------------------------------------------
// Rectangle fills. Boxes are axis-aligned, so these skip the per-pixel
// point-in-polygon test the static region helpers need, and the blur is a
// separable running sum: O(area) regardless of radius.
// ---------------------------------------------------------------------------
inline void black_rect(uint8_t* px, int w, int h, int ch, const Rect& r) {
    if (!px || !r.valid() || r.x1 > w || r.y1 > h) return;
    const size_t rowBytes = static_cast<size_t>(r.x1 - r.x0) * ch;
    for (int y = r.y0; y < r.y1; ++y)
        std::memset(px + (static_cast<size_t>(y) * w + r.x0) * ch, 0, rowBytes);
}

inline void pixelate_rect(uint8_t* px, int w, int h, int ch, const Rect& r, int block) {
    if (!px || !r.valid() || r.x1 > w || r.y1 > h || ch <= 0 || ch > 4) return;
    block = std::max(1, block);
    for (int by = r.y0; by < r.y1; by += block) {
        const int ey = std::min(by + block, r.y1);
        for (int bx = r.x0; bx < r.x1; bx += block) {
            const int ex = std::min(bx + block, r.x1);
            long sum[4] = {0, 0, 0, 0};
            const long n = static_cast<long>(ey - by) * (ex - bx);
            for (int y = by; y < ey; ++y) {
                const uint8_t* p = px + (static_cast<size_t>(y) * w + bx) * ch;
                for (int x = bx; x < ex; ++x, p += ch)
                    for (int c = 0; c < ch; ++c) sum[c] += p[c];
            }
            uint8_t avg[4];
            for (int c = 0; c < ch; ++c) avg[c] = static_cast<uint8_t>(sum[c] / n);
            for (int y = by; y < ey; ++y) {
                uint8_t* p = px + (static_cast<size_t>(y) * w + bx) * ch;
                for (int x = bx; x < ex; ++x, p += ch)
                    for (int c = 0; c < ch; ++c) p[c] = avg[c];
            }
        }
    }
}

// Box blur of the rectangle, edges clamped to the rectangle. Two passes
// (horizontal then vertical) of a running sum; `passes` > 1 approaches a
// Gaussian and removes the blocky look of a single box pass.
inline void blur_rect(uint8_t* px, int w, int h, int ch, const Rect& r, int radius,
                      int passes = 2) {
    if (!px || !r.valid() || r.x1 > w || r.y1 > h || ch <= 0) return;
    radius = std::max(1, radius);
    const int rw = r.x1 - r.x0, rh = r.y1 - r.y0;
    std::vector<uint8_t> line(static_cast<size_t>(std::max(rw, rh)) * ch);
    const int k = 2 * radius + 1;

    auto run = [&](uint8_t* start, int len, size_t step) {
        // Copy the line out, then write the running mean back.
        for (int i = 0; i < len; ++i)
            std::memcpy(&line[static_cast<size_t>(i) * ch], start + i * step, ch);
        for (int c = 0; c < ch; ++c) {
            long acc = 0;
            for (int i = -radius; i <= radius; ++i)
                acc += line[static_cast<size_t>(std::clamp(i, 0, len - 1)) * ch + c];
            for (int i = 0; i < len; ++i) {
                start[i * step + c] = static_cast<uint8_t>(acc / k);
                const int out = std::clamp(i - radius, 0, len - 1);
                const int in = std::clamp(i + radius + 1, 0, len - 1);
                acc += line[static_cast<size_t>(in) * ch + c] - line[static_cast<size_t>(out) * ch + c];
            }
        }
    };

    for (int p = 0; p < std::max(1, passes); ++p) {
        for (int y = r.y0; y < r.y1; ++y)
            run(px + (static_cast<size_t>(y) * w + r.x0) * ch, rw, static_cast<size_t>(ch));
        for (int x = r.x0; x < r.x1; ++x)
            run(px + (static_cast<size_t>(r.y0) * w + x) * ch, rh, static_cast<size_t>(w) * ch);
    }
}

// ---------------------------------------------------------------------------
// Box store: recent boxes per stream, looked up by frame pts.
// ---------------------------------------------------------------------------
class BoxStore {
public:
    explicit BoxStore(uint64_t hold_usec = 400000, std::size_t max_sets = 64)
        : hold_(hold_usec), max_(max_sets) {}

    // Record the boxes one event reported for (stream, pts). Several events for
    // the same pts (a detector and a face recogniser, say) accumulate.
    void add(int stream, uint64_t pts, const std::vector<Box>& boxes) {
        if (boxes.empty()) return;
        auto& q = streams_[stream];
        for (auto& s : q) {
            if (s.pts == pts) { s.boxes.insert(s.boxes.end(), boxes.begin(), boxes.end()); return; }
        }
        q.push_back({pts, boxes});
        while (q.size() > max_) q.pop_front();
    }

    // Every box within `hold` of pts on this stream. Sets older than the window
    // are dropped as a side effect, so memory stays bounded to recent frames.
    std::vector<Box> lookup(int stream, uint64_t pts) {
        std::vector<Box> out;
        auto it = streams_.find(stream);
        if (it == streams_.end()) return out;
        auto& q = it->second;
        while (!q.empty() && q.front().pts + hold_ < pts) q.pop_front();
        for (const auto& s : q) {
            const uint64_t d = s.pts > pts ? s.pts - pts : pts - s.pts;
            if (d <= hold_) out.insert(out.end(), s.boxes.begin(), s.boxes.end());
        }
        return out;
    }

    std::size_t sets(int stream) const {
        auto it = streams_.find(stream);
        return it == streams_.end() ? 0 : it->second.size();
    }

private:
    struct Set { uint64_t pts; std::vector<Box> boxes; };
    uint64_t hold_;
    std::size_t max_;
    std::map<int, std::deque<Set>> streams_;
};

}  // namespace privacy
}  // namespace zm
