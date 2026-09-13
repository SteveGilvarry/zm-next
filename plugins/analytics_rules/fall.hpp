#pragma once
// fall.hpp — pose-based fall / person-down detection with no model.
//
// Consumes detect_pose's per-person COCO-17 keypoints. Three posture signals per
// frame, each computed only from keypoints the pose model is confident about:
//   - torso tilt : the mid-shoulder -> mid-hip vector is more than `tilt_deg`
//                  from vertical (a standing or sitting person is near 0).
//   - wide bbox  : bbox width / height >= `aspect` (a lying body is wider than tall).
//   - head low   : the head (nose, else eyes/ears) is level with or below the
//                  mid-hip in image y (image y grows downward).
// A person is "down" on a frame when at least `min_signals` of the signals that
// could be computed agree. The rule fires once when a person stays down for
// `seconds`, has moved less than `max_drift` bbox-heights while down (a fallen
// person is still; someone crawling or doing push-ups is not), and, when
// `require_upright` is set, was seen upright within `upright_window_sec` before
// going down (so someone already lying on a sofa when the camera starts is not a
// fall). It re-arms after the person is seen upright again.
//
// detect_pose events carry no track id, so people are followed across frames by
// greedy nearest-centre matching within a stream (`associate`). That is enough
// for the handful of people a fall rule cares about; it is not a tracker.
//
// Pure header, no plugin ABI, so tests/test_fall.cpp exercises it directly.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace zm::analytics::fall {

inline constexpr double kPi = 3.14159265358979323846;

// COCO-17 keypoint order, as detect_pose emits by default.
enum Kp : int {
    Nose = 0, LEye = 1, REye = 2, LEar = 3, REar = 4,
    LShoulder = 5, RShoulder = 6, LHip = 11, RHip = 12, KpCount = 17
};

struct Keypoint { float x = 0, y = 0, v = 0; };

struct Pose {
    float x = 0, y = 0, w = 0, h = 0;   // bbox, source pixels
    std::vector<Keypoint> kpts;         // COCO-17 order
};

struct Params {
    double seconds = 2.0;             // continuously down before firing
    float tilt_deg = 55.0f;           // torso angle from vertical that counts as tilted
    float aspect = 1.0f;              // bbox w/h at or above this counts as wide
    int min_signals = 2;              // signals that must agree
    float kp_conf = 0.5f;             // keypoint visibility needed to use it
    float max_drift = 0.5f;           // max centre movement while down, in bbox heights
    bool require_upright = true;      // must have been upright shortly before
    double upright_window_sec = 5.0;  // how recently
};

struct Signals {
    bool tilt = false, wide = false, head_low = false;
    int computed = 0;   // how many of the three had enough keypoints
    int agree = 0;      // how many said "down"
    float tilt_deg = 0; // measured torso angle, when computed
};

namespace detail {
inline bool vis(const Pose& p, int k, float conf) {
    return k < static_cast<int>(p.kpts.size()) && p.kpts[k].v >= conf;
}
// Midpoint of a left/right pair; falls back to whichever side is visible.
inline bool mid(const Pose& p, int a, int b, float conf, float& mx, float& my) {
    const bool va = vis(p, a, conf), vb = vis(p, b, conf);
    if (va && vb) { mx = (p.kpts[a].x + p.kpts[b].x) / 2; my = (p.kpts[a].y + p.kpts[b].y) / 2; return true; }
    if (va) { mx = p.kpts[a].x; my = p.kpts[a].y; return true; }
    if (vb) { mx = p.kpts[b].x; my = p.kpts[b].y; return true; }
    return false;
}
}  // namespace detail

inline Signals posture(const Pose& p, const Params& prm) {
    Signals s;
    float sx, sy, hx, hy;
    const bool haveShoulders = detail::mid(p, LShoulder, RShoulder, prm.kp_conf, sx, sy);
    const bool haveHips = detail::mid(p, LHip, RHip, prm.kp_conf, hx, hy);

    if (haveShoulders && haveHips) {
        const float dx = hx - sx, dy = hy - sy;
        if (std::abs(dx) + std::abs(dy) > 1e-3f) {
            // 0 deg = hips straight below shoulders; 90 = horizontal; 180 = inverted.
            s.tilt_deg = static_cast<float>(std::atan2(std::abs(dx), dy) * 180.0 / kPi);
            s.tilt = s.tilt_deg >= prm.tilt_deg;
            s.computed++;
            if (s.tilt) s.agree++;
        }
    }

    if (p.h > 0) {
        s.wide = (p.w / p.h) >= prm.aspect;
        s.computed++;
        if (s.wide) s.agree++;
    }

    if (haveHips) {
        float headY = 0;
        bool haveHead = detail::vis(p, Nose, prm.kp_conf);
        if (haveHead) headY = p.kpts[Nose].y;
        else {
            float ex, ey;
            haveHead = detail::mid(p, LEye, REye, prm.kp_conf, ex, ey) ||
                       detail::mid(p, LEar, REar, prm.kp_conf, ex, ey);
            headY = ey;
        }
        if (haveHead) {
            s.head_low = headY >= hy;
            s.computed++;
            if (s.head_low) s.agree++;
        }
    }
    return s;
}

// A person is down when enough signals agree. With fewer signals computed than
// min_signals, all computed signals must agree and at least one must exist.
inline bool is_down(const Signals& s, const Params& prm) {
    if (s.computed == 0) return false;
    const int need = std::min(prm.min_signals, s.computed);
    return s.agree >= need;
}
// Upright means no signal says down. Mixed evidence is neither, so it neither
// starts a fall nor re-arms a fired one.
inline bool is_upright(const Signals& s) { return s.computed > 0 && s.agree == 0; }

// Per-person state across frames.
struct Person {
    int id = 0;
    float cx = 0, cy = 0, h = 0;   // last centre and bbox height
    uint64_t last_seen = 0;
    bool seen_upright = false;
    uint64_t upright_pts = 0;      // last frame this person was upright
    bool down = false;
    uint64_t down_pts = 0;         // when the current down episode started
    float down_cx = 0, down_cy = 0;
    bool fired = false;            // fired for this episode; re-armed by upright
};

struct Fire {
    bool fire = false;
    double down_sec = 0;
    float drift = 0;               // centre movement while down, in bbox heights
};

inline Fire step(Person& st, const Pose& p, uint64_t pts, const Params& prm) {
    Fire f;
    const Signals s = posture(p, prm);
    const float cx = p.x + p.w / 2, cy = p.y + p.h / 2;
    const float scale = std::max(1.0f, std::max(p.h, p.w));

    if (is_upright(s)) {
        st.seen_upright = true;
        st.upright_pts = pts;
        st.down = false;
        st.fired = false;
    } else if (is_down(s, prm)) {
        if (!st.down) {
            st.down = true;
            st.down_pts = pts;
            st.down_cx = cx;
            st.down_cy = cy;
        }
        const double down_sec = pts >= st.down_pts ? (pts - st.down_pts) / 1e6 : 0.0;
        const float drift = std::hypot(cx - st.down_cx, cy - st.down_cy) / scale;
        if (drift > prm.max_drift) {
            // Moving along the ground: restart the clock from here.
            st.down_pts = pts; st.down_cx = cx; st.down_cy = cy;
        } else if (!st.fired && down_sec >= prm.seconds) {
            const bool transitioned =
                !prm.require_upright ||
                (st.seen_upright && st.down_pts >= st.upright_pts &&
                 (st.down_pts - st.upright_pts) / 1e6 <= prm.upright_window_sec);
            if (transitioned) {
                st.fired = true;
                f.fire = true;
                f.down_sec = down_sec;
                f.drift = drift;
            }
        }
    }
    st.cx = cx; st.cy = cy; st.h = p.h; st.last_seen = pts;
    return f;
}

// Match this frame's poses to known people by nearest centre, greedy, within
// `max_dist` bbox-heights. Unmatched poses become new people. Returns, for each
// pose, the index into `people`. People not seen for `stale_usec` are dropped
// first.
inline std::vector<std::size_t> associate(std::vector<Person>& people,
                                          const std::vector<Pose>& poses,
                                          uint64_t pts, int& next_id,
                                          float max_dist = 1.0f,
                                          uint64_t stale_usec = 10ull * 1000000ull) {
    people.erase(std::remove_if(people.begin(), people.end(),
                                [&](const Person& q) { return pts >= q.last_seen && pts - q.last_seen > stale_usec; }),
                 people.end());

    struct Cand { float d; std::size_t pose, person; };
    std::vector<Cand> cands;
    for (std::size_t i = 0; i < poses.size(); ++i) {
        const auto& p = poses[i];
        const float cx = p.x + p.w / 2, cy = p.y + p.h / 2;
        const float scale = std::max(1.0f, std::max(p.h, p.w));
        for (std::size_t j = 0; j < people.size(); ++j) {
            const float d = std::hypot(cx - people[j].cx, cy - people[j].cy) / scale;
            if (d <= max_dist) cands.push_back({d, i, j});
        }
    }
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.d < b.d; });

    const std::size_t none = std::numeric_limits<std::size_t>::max();
    std::vector<std::size_t> out(poses.size(), none);
    std::vector<bool> taken(people.size(), false);
    for (const auto& c : cands) {
        if (out[c.pose] != none || taken[c.person]) continue;
        out[c.pose] = c.person;
        taken[c.person] = true;
    }
    for (std::size_t i = 0; i < poses.size(); ++i) {
        if (out[i] != none) continue;
        Person q;
        q.id = next_id++;
        const auto& p = poses[i];
        q.cx = p.x + p.w / 2; q.cy = p.y + p.h / 2; q.h = p.h; q.last_seen = pts;
        people.push_back(q);
        out[i] = people.size() - 1;
    }
    return out;
}

}  // namespace zm::analytics::fall
