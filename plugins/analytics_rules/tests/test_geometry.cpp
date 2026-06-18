#include "geometry.hpp"

#include <gtest/gtest.h>

using zm::analytics::cross;
using zm::analytics::intrusion_step;
using zm::analytics::loiter_step;
using zm::analytics::point_in_polygon;
using zm::analytics::Pt;
using zm::analytics::segments_intersect;
using zm::analytics::ZoneState;

namespace {
// A unit square (0,0)-(10,10).
std::vector<Pt> square() {
    return {{0, 0}, {10, 0}, {10, 10}, {0, 10}};
}
}  // namespace

// ---------------------------------------------------------------------------
// point_in_polygon
// ---------------------------------------------------------------------------
TEST(PointInPolygon, Inside) {
    EXPECT_TRUE(point_in_polygon(square(), Pt{5, 5}));
    EXPECT_TRUE(point_in_polygon(square(), Pt{1, 9}));
}

TEST(PointInPolygon, Outside) {
    EXPECT_FALSE(point_in_polygon(square(), Pt{-1, 5}));
    EXPECT_FALSE(point_in_polygon(square(), Pt{15, 5}));
    EXPECT_FALSE(point_in_polygon(square(), Pt{5, 20}));
}

TEST(PointInPolygon, OnEdge) {
    EXPECT_TRUE(point_in_polygon(square(), Pt{0, 5}));    // left edge
    EXPECT_TRUE(point_in_polygon(square(), Pt{10, 5}));   // right edge
    EXPECT_TRUE(point_in_polygon(square(), Pt{5, 0}));    // bottom edge
    EXPECT_TRUE(point_in_polygon(square(), Pt{0, 0}));    // corner
}

TEST(PointInPolygon, DegeneratePolygon) {
    std::vector<Pt> line = {{0, 0}, {10, 0}};  // < 3 verts
    EXPECT_FALSE(point_in_polygon(line, Pt{5, 0}));
}

TEST(PointInPolygon, ConcavePolygon) {
    // An L / arrow shape: notch cut out around (8,5).
    std::vector<Pt> poly = {{0, 0}, {10, 0}, {10, 10}, {6, 10},
                            {6, 4},  {4, 4},  {4, 10},  {0, 10}};
    EXPECT_TRUE(point_in_polygon(poly, Pt{2, 8}));
    EXPECT_TRUE(point_in_polygon(poly, Pt{8, 8}));
    EXPECT_FALSE(point_in_polygon(poly, Pt{5, 8}));  // in the notch
}

// ---------------------------------------------------------------------------
// segments_intersect
// ---------------------------------------------------------------------------
TEST(SegmentsIntersect, Crossing) {
    EXPECT_TRUE(segments_intersect(Pt{0, 0}, Pt{10, 10}, Pt{0, 10}, Pt{10, 0}));
}

TEST(SegmentsIntersect, ParallelNonOverlapping) {
    EXPECT_FALSE(segments_intersect(Pt{0, 0}, Pt{10, 0}, Pt{0, 5}, Pt{10, 5}));
}

TEST(SegmentsIntersect, Collinear) {
    EXPECT_TRUE(segments_intersect(Pt{0, 0}, Pt{10, 0}, Pt{5, 0}, Pt{15, 0}));
    EXPECT_FALSE(segments_intersect(Pt{0, 0}, Pt{4, 0}, Pt{6, 0}, Pt{10, 0}));
}

TEST(SegmentsIntersect, Touching) {
    // Endpoint of one lies on the other (T-junction).
    EXPECT_TRUE(segments_intersect(Pt{0, 0}, Pt{10, 0}, Pt{5, 0}, Pt{5, 10}));
}

TEST(SegmentsIntersect, Disjoint) {
    EXPECT_FALSE(segments_intersect(Pt{0, 0}, Pt{1, 1}, Pt{5, 5}, Pt{6, 6}));
}

// ---------------------------------------------------------------------------
// cross sign (line-cross direction)
// ---------------------------------------------------------------------------
TEST(Cross, SignLeftRight) {
    // Directed line a(0,0)->b(0,10) (pointing +y). A point to the +x side is
    // on the right (cross < 0); to the -x side is on the left (cross > 0).
    Pt a{0, 0}, b{0, 10};
    EXPECT_LT(cross(a, b, Pt{5, 5}), 0.0f);   // right of a->b
    EXPECT_GT(cross(a, b, Pt{-5, 5}), 0.0f);  // left of a->b
    EXPECT_FLOAT_EQ(cross(a, b, Pt{0, 5}), 0.0f);  // collinear
}

// ---------------------------------------------------------------------------
// intrusion latch: fire once per entry, re-arm on exit
// ---------------------------------------------------------------------------
TEST(IntrusionStep, FiresOnceThenReArms) {
    ZoneState s;
    EXPECT_FALSE(intrusion_step(s, false));  // start outside
    EXPECT_TRUE(intrusion_step(s, true));    // enter -> fire
    EXPECT_FALSE(intrusion_step(s, true));   // still inside -> no fire
    EXPECT_FALSE(intrusion_step(s, false));  // exit
    EXPECT_TRUE(intrusion_step(s, true));    // re-enter -> fire again
}

// ---------------------------------------------------------------------------
// loiter latch: fire once after >= S sec dwell, re-arm on exit
// ---------------------------------------------------------------------------
TEST(LoiterStep, FiresAfterDwell) {
    ZoneState s;
    const double secs = 3.0;
    // Enter at t=0s.
    EXPECT_FALSE(loiter_step(s, true, 0, secs).fire);
    // Still inside at t=2s: not enough.
    EXPECT_FALSE(loiter_step(s, true, 2'000'000, secs).fire);
    // At t=3s: fire, dwell == 3.
    auto r = loiter_step(s, true, 3'000'000, secs);
    EXPECT_TRUE(r.fire);
    EXPECT_DOUBLE_EQ(r.dwell_sec, 3.0);
    // No re-fire while still inside.
    EXPECT_FALSE(loiter_step(s, true, 5'000'000, secs).fire);
}

TEST(LoiterStep, ReArmsOnExit) {
    ZoneState s;
    const double secs = 2.0;
    loiter_step(s, true, 0, secs);
    EXPECT_TRUE(loiter_step(s, true, 2'000'000, secs).fire);
    EXPECT_FALSE(loiter_step(s, false, 3'000'000, secs).fire);  // exit, re-arm
    loiter_step(s, true, 4'000'000, secs);                      // re-enter
    EXPECT_TRUE(loiter_step(s, true, 6'000'000, secs).fire);    // fires again
}
