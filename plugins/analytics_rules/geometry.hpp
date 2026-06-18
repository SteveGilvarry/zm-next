// Pure geometry + rule-state helpers for the analytics_rules plugin.
//
// No plugin ABI here on purpose: everything in zm::analytics is free of the
// host API so it can be unit-tested standalone (see tests/test_geometry.cpp).
#pragma once

#include <cstdint>
#include <vector>

namespace zm::analytics {

// 2D point. Used for object ground positions and polygon/line vertices.
struct Pt {
    float x = 0.0f;
    float y = 0.0f;
};

// Signed cross product of (a-o) x (b-o). Sign tells which side of the directed
// line o->a the point b lies on: >0 left, <0 right, ==0 collinear. Used for
// line-cross direction tests.
inline float cross(Pt o, Pt a, Pt b) {
    return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
}

// Ray-casting point-in-polygon test. `poly` is an ordered (open) ring of
// vertices; the closing edge from poly.back() to poly.front() is implied.
// Returns true if p is strictly inside or on the boundary. The classic
// half-open ray test treats edges consistently so that a point exactly on an
// edge is reported deterministically as inside.
inline bool point_in_polygon(const std::vector<Pt>& poly, Pt p) {
    const std::size_t n = poly.size();
    if (n < 3) return false;

    bool inside = false;
    for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
        const Pt& a = poly[i];
        const Pt& b = poly[j];

        // On-segment check: treat a point lying on any edge as inside.
        const float d = cross(a, b, p);  // 0 => collinear with edge a-b
        if (d == 0.0f) {
            const bool within_x = (p.x >= (a.x < b.x ? a.x : b.x)) &&
                                  (p.x <= (a.x > b.x ? a.x : b.x));
            const bool within_y = (p.y >= (a.y < b.y ? a.y : b.y)) &&
                                  (p.y <= (a.y > b.y ? a.y : b.y));
            if (within_x && within_y) return true;
        }

        // Ray crossing test (ray cast toward +x).
        const bool crosses = ((a.y > p.y) != (b.y > p.y)) &&
                             (p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x);
        if (crosses) inside = !inside;
    }
    return inside;
}

// Orientation of ordered triplet (p,q,r): 0 collinear, 1 clockwise,
// 2 counter-clockwise.
inline int orientation(Pt p, Pt q, Pt r) {
    const float v = (q.y - p.y) * (r.x - q.x) - (q.x - p.x) * (r.y - q.y);
    if (v == 0.0f) return 0;
    return (v > 0.0f) ? 1 : 2;
}

// True if point q lies on segment pr (assuming the three are collinear).
inline bool on_segment(Pt p, Pt q, Pt r) {
    return q.x <= (p.x > r.x ? p.x : r.x) && q.x >= (p.x < r.x ? p.x : r.x) &&
           q.y <= (p.y > r.y ? p.y : r.y) && q.y >= (p.y < r.y ? p.y : r.y);
}

// True if segment ab intersects segment cd (including collinear-overlap and
// endpoint-touch cases). Standard orientation-based test.
inline bool segments_intersect(Pt a, Pt b, Pt c, Pt d) {
    const int o1 = orientation(a, b, c);
    const int o2 = orientation(a, b, d);
    const int o3 = orientation(c, d, a);
    const int o4 = orientation(c, d, b);

    if (o1 != o2 && o3 != o4) return true;

    if (o1 == 0 && on_segment(a, c, b)) return true;
    if (o2 == 0 && on_segment(a, d, b)) return true;
    if (o3 == 0 && on_segment(c, a, d)) return true;
    if (o4 == 0 && on_segment(c, b, d)) return true;

    return false;
}

// Ground position of a detection bbox [x,y,w,h]: bottom-center point. This is
// the standard "where the object's feet touch the floor" anchor for footfall /
// spatial analytics.
inline Pt bbox_ground(float x, float y, float w, float h) {
    return Pt{x + w * 0.5f, y + h};
}

// ---------------------------------------------------------------------------
// Pure per-track rule state. These hold no ABI types; the plugin owns one
// instance per (rule, track) pair and drives the transitions.
// ---------------------------------------------------------------------------

// Latch for intrusion / loiter: tracks whether the object is currently counted
// as "inside" so we fire once per entry and re-arm only after an exit.
struct ZoneState {
    bool inside = false;          // currently inside the polygon
    bool fired = false;           // alarm already raised for this dwell/entry
    uint64_t enter_pts_usec = 0;  // pts when the object first entered (loiter clock)
};

// Result of feeding a new sample to a loiter rule.
struct LoiterResult {
    bool fire = false;       // raise the alarm now
    double dwell_sec = 0.0;  // continuous dwell time at fire moment
};

// Advance an intrusion latch with a new sample. Returns true exactly on the
// outside->inside transition (re-armed by a prior outside sample).
inline bool intrusion_step(ZoneState& s, bool now_inside) {
    bool fire = false;
    if (now_inside && !s.inside) fire = true;  // entered
    s.inside = now_inside;
    if (!now_inside) s.fired = false;  // re-arm on exit
    return fire;
}

// Advance a loiter latch with a new sample at time `pts_usec`. Fires once when
// the object has been continuously inside for >= seconds. Re-arms on exit.
inline LoiterResult loiter_step(ZoneState& s, bool now_inside, uint64_t pts_usec,
                                double seconds) {
    LoiterResult r;
    if (now_inside) {
        if (!s.inside) {  // just entered: start the dwell clock
            s.inside = true;
            s.fired = false;
            s.enter_pts_usec = pts_usec;
        } else if (!s.fired) {
            const double dwell =
                (pts_usec >= s.enter_pts_usec)
                    ? static_cast<double>(pts_usec - s.enter_pts_usec) / 1e6
                    : 0.0;
            if (dwell >= seconds) {
                s.fired = true;
                r.fire = true;
                r.dwell_sec = dwell;
            }
        }
    } else {
        s.inside = false;  // exited: re-arm
        s.fired = false;
    }
    return r;
}

}  // namespace zm::analytics
