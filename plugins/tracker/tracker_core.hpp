// tracker_core.hpp — pure, dependency-free multi-object tracking logic.
//
// SORT-style IoU tracker (no Kalman for v1). Lives in namespace zm::tracker and
// has NO dependency on the plugin ABI or EventBus, so it is unit-testable in
// isolation. tracker.cpp wraps this with EventBus subscription + JSON parsing.
//
// Algorithm (per detection batch / update() call):
//   1. Compute IoU between every existing track's last bbox and every new det.
//   2. Greedily match the highest IoU pair above iou_threshold, one-to-one,
//      removing matched track/det from further consideration.
//   3. Matched tracks: update bbox, ++hits, time_since_update = 0.
//   4. Unmatched dets: spawn a new tentative track (fresh incrementing id),
//      hits = 1, time_since_update = 0.
//   5. Unmatched tracks: ++time_since_update; removed when it exceeds max_age.
//
// update() returns a vector parallel to the input dets: the emitted track_id for
// each detection. A track is only "confirmed" (emits its real id) once it has
// hits >= min_hits; tentative tracks (and any det that failed to produce a
// confirmed track this frame) emit 0. id 0 is therefore reserved as "no track".

#pragma once

#include <vector>
#include <algorithm>
#include <cmath>
#include <cstddef>

namespace zm::tracker {

// A detection in [x, y, w, h] box form (top-left origin, width/height).
struct Det {
    float x = 0.f;
    float y = 0.f;
    float w = 0.f;
    float h = 0.f;
    int class_id = -1;
    float confidence = 0.f;
    // Optional appearance embedding (e.g. a normalized colour histogram from the
    // detector). Empty when ReID is off; association then falls back to IoU only.
    std::vector<float> embedding;
};

// A live track. `id` is the persistent identifier assigned across frames.
struct Track {
    int id = 0;
    float x = 0.f;
    float y = 0.f;
    float w = 0.f;
    float h = 0.f;
    int hits = 0;               // total number of matched detections
    int time_since_update = 0;  // frames since last successful match
    int class_id = -1;
    std::vector<float> embedding;  // EMA-smoothed appearance over matched dets
};

// Cosine similarity of two equal-length vectors in [-1, 1]; 0 if either is empty
// or a different length (treated as "no appearance information").
inline float cosine_sim(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.empty() || a.size() != b.size()) return 0.f;
    float dot = 0.f, na = 0.f, nb = 0.f;
    for (std::size_t i = 0; i < a.size(); ++i) {
        dot += a[i] * b[i];
        na += a[i] * a[i];
        nb += b[i] * b[i];
    }
    if (na <= 0.f || nb <= 0.f) return 0.f;
    return dot / (std::sqrt(na) * std::sqrt(nb));
}

// L2-normalize in place (no-op for an empty or zero vector).
inline void l2_normalize(std::vector<float>& v) {
    float n = 0.f;
    for (float x : v) n += x * x;
    if (n <= 0.f) return;
    n = std::sqrt(n);
    for (float& x : v) x /= n;
}

// IoU between a detection box and a track's last box. Returns 0 for empty/degenerate
// boxes or no overlap; otherwise intersection / union in [0, 1].
inline float iou(float ax, float ay, float aw, float ah,
                 float bx, float by, float bw, float bh) {
    if (aw <= 0.f || ah <= 0.f || bw <= 0.f || bh <= 0.f) return 0.f;
    const float ax2 = ax + aw, ay2 = ay + ah;
    const float bx2 = bx + bw, by2 = by + bh;
    const float ix1 = std::max(ax, bx);
    const float iy1 = std::max(ay, by);
    const float ix2 = std::min(ax2, bx2);
    const float iy2 = std::min(ay2, by2);
    const float iw = ix2 - ix1;
    const float ih = iy2 - iy1;
    if (iw <= 0.f || ih <= 0.f) return 0.f;
    const float inter = iw * ih;
    const float uni = aw * ah + bw * bh - inter;
    if (uni <= 0.f) return 0.f;
    return inter / uni;
}

inline float iou(const Det& d, const Track& t) {
    return iou(d.x, d.y, d.w, d.h, t.x, t.y, t.w, t.h);
}

class Tracker {
public:
    Tracker() = default;
    Tracker(float iou_threshold, int max_age, int min_hits,
            bool class_gated = true, float appearance_threshold = 0.f,
            float appearance_weight = 0.f, float embed_alpha = 0.1f)
        : iou_threshold_(iou_threshold), max_age_(max_age), min_hits_(min_hits),
          class_gated_(class_gated),
          appearance_threshold_(appearance_threshold),
          appearance_weight_(appearance_weight), embed_alpha_(embed_alpha) {}

    void set_params(float iou_threshold, int max_age, int min_hits,
                    bool class_gated = true, float appearance_threshold = 0.f,
                    float appearance_weight = 0.f, float embed_alpha = 0.1f) {
        iou_threshold_ = iou_threshold;
        max_age_ = max_age;
        min_hits_ = min_hits;
        class_gated_ = class_gated;
        appearance_threshold_ = appearance_threshold;
        appearance_weight_ = appearance_weight;
        embed_alpha_ = embed_alpha;
    }

    // Advance the tracker by one detection batch. Returns a vector parallel to
    // `dets`: the track_id assigned to each detection (0 == no confirmed track).
    std::vector<int> update(const std::vector<Det>& dets) {
        const std::size_t nT = tracks_.size();
        const std::size_t nD = dets.size();

        // Age every track up front; matches will reset to 0 below.
        for (auto& t : tracks_) t.time_since_update++;

        std::vector<int> assigned(nD, 0);           // det -> track_id (output)
        std::vector<int> det_to_track(nD, -1);      // det index -> tracks_ index
        std::vector<bool> det_matched(nD, false);
        std::vector<bool> track_matched(nT, false);

        // Greedy highest-IoU one-to-one association.
        if (nT > 0 && nD > 0) {
            struct Pair { float score; std::size_t ti; std::size_t di; };
            std::vector<Pair> pairs;
            pairs.reserve(nT * nD);
            for (std::size_t ti = 0; ti < nT; ++ti) {
                for (std::size_t di = 0; di < nD; ++di) {
                    // Class-gated association: never let a track of one class
                    // re-acquire a detection of another (a "person" track must
                    // not poach a passing "car"). This is the first guard against
                    // a single track_id spanning two distinct objects. (-1 is
                    // "unknown" and matches anything so untyped dets still track.)
                    if (class_gated_ && dets[di].class_id != tracks_[ti].class_id &&
                        dets[di].class_id != -1 && tracks_[ti].class_id != -1)
                        continue;
                    const float spatial = iou(dets[di], tracks_[ti]);
                    if (spatial < iou_threshold_) continue;
                    // Appearance gate (ReID): when both carry an embedding, reject
                    // the pair if they don't look alike — so a lingering track of
                    // one car can't poach a *different* car of the same class that
                    // happens to overlap. Score blends IoU with appearance.
                    float score = spatial;
                    if (appearance_threshold_ > 0.f &&
                        !dets[di].embedding.empty() &&
                        dets[di].embedding.size() == tracks_[ti].embedding.size()) {
                        const float app =
                            cosine_sim(dets[di].embedding, tracks_[ti].embedding);
                        if (app < appearance_threshold_) continue;
                        score = (1.f - appearance_weight_) * spatial +
                                appearance_weight_ * app;
                    }
                    pairs.push_back({score, ti, di});
                }
            }
            std::sort(pairs.begin(), pairs.end(),
                      [](const Pair& a, const Pair& b) { return a.score > b.score; });
            for (const auto& p : pairs) {
                if (track_matched[p.ti] || det_matched[p.di]) continue;
                track_matched[p.ti] = true;
                det_matched[p.di] = true;
                det_to_track[p.di] = static_cast<int>(p.ti);
                auto& t = tracks_[p.ti];
                const auto& d = dets[p.di];
                t.x = d.x; t.y = d.y; t.w = d.w; t.h = d.h;
                t.class_id = d.class_id;
                t.hits++;
                t.time_since_update = 0;
                // EMA-smooth the appearance so a single noisy frame can't swing
                // the track's identity; keep it unit-norm for cosine compares.
                if (!d.embedding.empty()) {
                    if (t.embedding.size() != d.embedding.size()) {
                        t.embedding = d.embedding;
                    } else {
                        for (std::size_t k = 0; k < t.embedding.size(); ++k)
                            t.embedding[k] = (1.f - embed_alpha_) * t.embedding[k] +
                                             embed_alpha_ * d.embedding[k];
                    }
                    l2_normalize(t.embedding);
                }
            }
        }

        // Unmatched detections spawn new tentative tracks.
        for (std::size_t di = 0; di < nD; ++di) {
            if (det_matched[di]) continue;
            Track t;
            t.id = next_id_++;
            const auto& d = dets[di];
            t.x = d.x; t.y = d.y; t.w = d.w; t.h = d.h;
            t.class_id = d.class_id;
            t.hits = 1;
            t.time_since_update = 0;
            t.embedding = d.embedding;  // seed appearance from the first detection
            tracks_.push_back(t);
            det_to_track[di] = static_cast<int>(tracks_.size() - 1);
        }

        // Emit confirmed track ids (hits >= min_hits), else 0.
        for (std::size_t di = 0; di < nD; ++di) {
            const int idx = det_to_track[di];
            if (idx >= 0 && tracks_[static_cast<std::size_t>(idx)].hits >= min_hits_)
                assigned[di] = tracks_[static_cast<std::size_t>(idx)].id;
            else
                assigned[di] = 0;
        }

        // Prune stale tracks.
        tracks_.erase(
            std::remove_if(tracks_.begin(), tracks_.end(),
                           [this](const Track& t) { return t.time_since_update > max_age_; }),
            tracks_.end());

        return assigned;
    }

    const std::vector<Track>& tracks() const { return tracks_; }
    std::size_t track_count() const { return tracks_.size(); }
    float iou_threshold() const { return iou_threshold_; }
    int max_age() const { return max_age_; }
    int min_hits() const { return min_hits_; }
    bool class_gated() const { return class_gated_; }
    float appearance_threshold() const { return appearance_threshold_; }

private:
    std::vector<Track> tracks_;
    int next_id_ = 1;            // 0 is reserved for "no track"
    float iou_threshold_ = 0.3f;
    int max_age_ = 30;
    int min_hits_ = 3;
    bool class_gated_ = true;
    float appearance_threshold_ = 0.f;  // min cosine sim to allow a match (0=off)
    float appearance_weight_ = 0.f;     // blend of appearance into match score
    float embed_alpha_ = 0.1f;          // EMA weight on each new embedding
};

}  // namespace zm::tracker
