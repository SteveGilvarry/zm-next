// tracker_core.hpp — pure, dependency-free multi-object tracking logic.
//
// OC-SORT-style tracker in namespace zm::tracker, with NO dependency on the
// plugin ABI or EventBus, so it is unit-testable in isolation. tracker.cpp wraps
// this with EventBus subscription + JSON parsing.
//
// Upgrade over the original greedy-IoU SORT (v1):
//   * Per-track constant-velocity KALMAN filter on the box centre → tracks are
//     matched against their PREDICTED position, so they survive occlusion gaps,
//     non-linear-ish motion and low detection cadence instead of fragmenting.
//   * OC-SORT heuristics:
//       - OCM (Observation-Centric Momentum): a velocity-direction-consistency
//         term added to the match score, so a track prefers detections that
//         continue its motion.
//       - OCR (Observation-Centric Recovery): a recovery pass that re-associates
//         still-unmatched tracks to leftover detections using the track's LAST
//         OBSERVATION box (not the drifted KF prediction).
//       - ORU (Observation-Centric Re-Update): on recovery, the velocity is
//         re-seeded from the observation gap before the KF update, correcting
//         drift accumulated while coasting.
//   * ByteTrack two-stage association: high-confidence detections associate
//     first, then leftover (low-confidence) detections recover unmatched tracks
//     in a second pass; only high-confidence leftovers spawn new tracks.
//
// All association passes apply the same gates (class gate + appearance/ReID gate
// + IoU threshold); only the candidate set and the box used for IoU differ.
//
// update() returns a vector parallel to the input dets: the emitted track_id for
// each detection (0 == no confirmed track). A track is "confirmed" once hits >=
// min_hits; id 0 is reserved as "no track".
//
// Pure C++/std, no new dependency.

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
    // Optional appearance embedding (e.g. an OSNet ReID vector, or a normalized
    // colour histogram). Empty when ReID is off; association then falls back to
    // motion/IoU only.
    std::vector<float> embedding;
};

// 1-D constant-velocity Kalman filter (position + velocity), used per axis on the
// box centre. Tiny, fixed-size, no matrices — F = [[1,1],[0,1]], H = [1,0].
struct Kf1D {
    float x = 0.f;     // position estimate
    float v = 0.f;     // velocity estimate
    float p00 = 10.f, p01 = 0.f, p11 = 1000.f;  // covariance (velocity unknown at init)

    void init(float pos) { x = pos; v = 0.f; p00 = 10.f; p01 = 0.f; p11 = 1000.f; }

    void predict(float q_pos, float q_vel) {
        x += v;  // constant-velocity step
        const float n00 = p00 + 2.f * p01 + p11 + q_pos;
        const float n01 = p01 + p11;
        const float n11 = p11 + q_vel;
        p00 = n00; p01 = n01; p11 = n11;
    }

    void update(float z, float r) {
        const float s = p00 + r;
        if (s <= 0.f) return;
        const float k0 = p00 / s;
        const float k1 = p01 / s;
        const float y = z - x;
        x += k0 * y;
        v += k1 * y;
        const float np00 = (1.f - k0) * p00;
        const float np01 = (1.f - k0) * p01;
        const float np11 = p11 - k1 * p01;
        p00 = np00; p01 = np01; p11 = np11;
    }
};

// A live track. `id` is the persistent identifier assigned across frames. x/y/w/h
// is the current best box estimate this frame (KF-predicted, then corrected on a
// match) so downstream consumers always read a sensible position even while the
// track is coasting through a gap.
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

    // Motion state (KF on box centre) + last observation (for OCR/OCM/ORU).
    Kf1D kx, ky;
    float lcx = 0.f, lcy = 0.f;  // centre of the last OBSERVED (matched) box
    float lw = 0.f, lh = 0.f;    // size of the last observed box
    bool has_obs = false;
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

// IoU between two boxes. Returns 0 for empty/degenerate boxes or no overlap.
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
            float appearance_weight = 0.f, float embed_alpha = 0.1f,
            float det_high_thresh = 0.f, float low_iou_threshold = 0.2f,
            float ocm_weight = 0.2f)
        : iou_threshold_(iou_threshold), max_age_(max_age), min_hits_(min_hits),
          class_gated_(class_gated),
          appearance_threshold_(appearance_threshold),
          appearance_weight_(appearance_weight), embed_alpha_(embed_alpha),
          det_high_thresh_(det_high_thresh), low_iou_threshold_(low_iou_threshold),
          ocm_weight_(ocm_weight) {}

    void set_params(float iou_threshold, int max_age, int min_hits,
                    bool class_gated = true, float appearance_threshold = 0.f,
                    float appearance_weight = 0.f, float embed_alpha = 0.1f,
                    float det_high_thresh = 0.f, float low_iou_threshold = 0.2f,
                    float ocm_weight = 0.2f) {
        iou_threshold_ = iou_threshold;
        max_age_ = max_age;
        min_hits_ = min_hits;
        class_gated_ = class_gated;
        appearance_threshold_ = appearance_threshold;
        appearance_weight_ = appearance_weight;
        embed_alpha_ = embed_alpha;
        det_high_thresh_ = det_high_thresh;
        low_iou_threshold_ = low_iou_threshold;
        ocm_weight_ = ocm_weight;
    }

    // Advance the tracker by one detection batch. Returns a vector parallel to
    // `dets`: the track_id assigned to each detection (0 == no confirmed track).
    std::vector<int> update(const std::vector<Det>& dets) {
        const std::size_t nT = tracks_.size();
        const std::size_t nD = dets.size();

        // Age + KF-predict every track up front; matches reset age below. The
        // predicted centre + last observed size becomes the track's box for IoU.
        for (auto& t : tracks_) {
            t.time_since_update++;
            t.kx.predict(q_pos_, q_vel_);
            t.ky.predict(q_pos_, q_vel_);
            t.x = t.kx.x - t.lw * 0.5f;
            t.y = t.ky.x - t.lh * 0.5f;
            t.w = t.lw;
            t.h = t.lh;
        }

        std::vector<int> det_to_track(nD, -1);
        std::vector<bool> det_matched(nD, false);
        std::vector<bool> track_matched(nT, false);

        // Confidence buckets (ByteTrack). A det is "low" only when a positive
        // high-threshold is configured AND it carries a real (>0) confidence
        // below it — so zero/unknown-confidence callers keep single-stage behaviour.
        std::vector<std::size_t> high, low;
        for (std::size_t di = 0; di < nD; ++di) {
            const float c = dets[di].confidence;
            if (det_high_thresh_ > 0.f && c > 0.f && c < det_high_thresh_)
                low.push_back(di);
            else
                high.push_back(di);
        }

        // Stage 1: high-confidence dets vs predicted track boxes.
        associate(dets, high, det_matched, track_matched, det_to_track,
                  /*use_last_obs=*/false, iou_threshold_, /*use_ocm=*/true);

        // Stage 2 (ByteTrack): leftover low-confidence dets recover unmatched
        // tracks (relaxed IoU). Low-conf dets never spawn new tracks.
        if (!low.empty())
            associate(dets, low, det_matched, track_matched, det_to_track,
                      /*use_last_obs=*/false, low_iou_threshold_, /*use_ocm=*/false);

        // Stage 3 (OCR): still-unmatched tracks vs still-unmatched high-conf dets,
        // scored on the track's LAST OBSERVATION box (not the drifted prediction).
        std::vector<std::size_t> high_left;
        for (std::size_t di : high) if (!det_matched[di]) high_left.push_back(di);
        if (!high_left.empty())
            associate(dets, high_left, det_matched, track_matched, det_to_track,
                      /*use_last_obs=*/true, iou_threshold_, /*use_ocm=*/false);

        // Unmatched HIGH-confidence detections spawn new tentative tracks.
        for (std::size_t di : high) {
            if (det_matched[di]) continue;
            Track t;
            t.id = next_id_++;
            const auto& d = dets[di];
            t.class_id = d.class_id;
            t.hits = 1;
            t.time_since_update = 0;
            const float cx = d.x + d.w * 0.5f, cy = d.y + d.h * 0.5f;
            t.kx.init(cx); t.ky.init(cy);
            t.lcx = cx; t.lcy = cy; t.lw = d.w; t.lh = d.h; t.has_obs = true;
            t.x = d.x; t.y = d.y; t.w = d.w; t.h = d.h;
            t.embedding = d.embedding;
            tracks_.push_back(t);
            det_to_track[di] = static_cast<int>(tracks_.size() - 1);
        }

        // Emit confirmed track ids (hits >= min_hits), else 0.
        std::vector<int> assigned(nD, 0);
        for (std::size_t di = 0; di < nD; ++di) {
            const int idx = det_to_track[di];
            if (idx >= 0 && tracks_[static_cast<std::size_t>(idx)].hits >= min_hits_)
                assigned[di] = tracks_[static_cast<std::size_t>(idx)].id;
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
    // Whether a (det, track) pair is allowed to associate, under the class gate
    // and the appearance/ReID gate. Shared by every association pass.
    bool gate_ok(const Det& d, const Track& t) const {
        if (class_gated_ && d.class_id != t.class_id &&
            d.class_id != -1 && t.class_id != -1)
            return false;
        if (appearance_threshold_ > 0.f && !d.embedding.empty() &&
            d.embedding.size() == t.embedding.size()) {
            if (cosine_sim(d.embedding, t.embedding) < appearance_threshold_)
                return false;
        }
        return true;
    }

    // Greedy highest-score one-to-one association over a candidate det subset.
    // `use_last_obs` scores IoU against the track's last observation box (OCR)
    // instead of its predicted box; `iou_thr` is the gate; `use_ocm` adds the
    // observation-centric momentum term. Updates det/track match state + the KF.
    void associate(const std::vector<Det>& dets, const std::vector<std::size_t>& cand,
                   std::vector<bool>& det_matched, std::vector<bool>& track_matched,
                   std::vector<int>& det_to_track, bool use_last_obs, float iou_thr,
                   bool use_ocm) {
        const std::size_t nT = tracks_.size();
        if (cand.empty() || nT == 0) return;
        struct Pair { float score; std::size_t ti; std::size_t di; };
        std::vector<Pair> pairs;
        for (std::size_t ti = 0; ti < nT; ++ti) {
            if (track_matched[ti]) continue;
            const Track& t = tracks_[ti];
            for (std::size_t di : cand) {
                if (det_matched[di]) continue;
                const Det& d = dets[di];
                if (!gate_ok(d, t)) continue;
                float spatial;
                if (use_last_obs)
                    spatial = iou(d.x, d.y, d.w, d.h,
                                  t.lcx - t.lw * 0.5f, t.lcy - t.lh * 0.5f, t.lw, t.lh);
                else
                    spatial = iou(d, t);
                if (spatial < iou_thr) continue;
                float score = spatial;
                if (appearance_threshold_ > 0.f && !d.embedding.empty() &&
                    d.embedding.size() == t.embedding.size()) {
                    const float app = cosine_sim(d.embedding, t.embedding);
                    score = (1.f - appearance_weight_) * spatial + appearance_weight_ * app;
                }
                if (use_ocm && ocm_weight_ > 0.f && t.has_obs) {
                    const float speed = std::sqrt(t.kx.v * t.kx.v + t.ky.v * t.ky.v);
                    const float dcx = d.x + d.w * 0.5f - t.lcx;
                    const float dcy = d.y + d.h * 0.5f - t.lcy;
                    const float dist = std::sqrt(dcx * dcx + dcy * dcy);
                    if (speed > 1e-3f && dist > 1e-3f) {
                        const float dotdir = (t.kx.v * dcx + t.ky.v * dcy) / (speed * dist);
                        score += ocm_weight_ * dotdir;  // [-w, +w], soft (never a gate)
                    }
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
            Track& t = tracks_[p.ti];
            const Det& d = dets[p.di];
            const float cx = d.x + d.w * 0.5f, cy = d.y + d.h * 0.5f;
            // ORU: on a recovery match after a gap, re-seed velocity from the
            // observation delta so the KF doesn't keep its stale coasting velocity.
            if (use_last_obs && t.time_since_update > 1 && t.has_obs) {
                const float g = static_cast<float>(t.time_since_update);
                t.kx.v = (cx - t.lcx) / g;
                t.ky.v = (cy - t.lcy) / g;
            }
            t.kx.update(cx, r_meas_);
            t.ky.update(cy, r_meas_);
            t.class_id = d.class_id;
            t.hits++;
            t.time_since_update = 0;
            t.lcx = cx; t.lcy = cy; t.lw = d.w; t.lh = d.h; t.has_obs = true;
            t.x = t.kx.x - d.w * 0.5f; t.y = t.ky.x - d.h * 0.5f; t.w = d.w; t.h = d.h;
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

    std::vector<Track> tracks_;
    int next_id_ = 1;            // 0 is reserved for "no track"
    float iou_threshold_ = 0.3f;
    int max_age_ = 30;
    int min_hits_ = 3;
    bool class_gated_ = true;
    float appearance_threshold_ = 0.f;
    float appearance_weight_ = 0.f;
    float embed_alpha_ = 0.1f;
    float det_high_thresh_ = 0.f;     // ByteTrack high/low split; 0 = single-stage
    float low_iou_threshold_ = 0.2f;  // relaxed IoU for the low-conf recovery pass
    float ocm_weight_ = 0.2f;         // observation-centric momentum weight
    // KF noise constants (px units on the box centre).
    float q_pos_ = 1.0f;
    float q_vel_ = 0.01f;
    float r_meas_ = 1.0f;
};

}  // namespace zm::tracker
