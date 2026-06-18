#include "../tracker_core.hpp"

#include <gtest/gtest.h>
#include <vector>

using namespace zm::tracker;

namespace {
Det makeDet(float x, float y, float w, float h, int cls = 0, float conf = 0.9f) {
    Det d;
    d.x = x; d.y = y; d.w = w; d.h = h;
    d.class_id = cls; d.confidence = conf;
    return d;
}
}  // namespace

TEST(IouTest, IdenticalBoxesIsOne) {
    EXPECT_FLOAT_EQ(iou(0, 0, 10, 10, 0, 0, 10, 10), 1.0f);
}

TEST(IouTest, NoOverlapIsZero) {
    EXPECT_FLOAT_EQ(iou(0, 0, 10, 10, 100, 100, 10, 10), 0.0f);
}

TEST(IouTest, HalfOverlapKnownValue) {
    // Two 10x10 boxes overlapping in a 5x10 strip.
    // inter = 5*10 = 50; union = 100 + 100 - 50 = 150; iou = 1/3.
    EXPECT_NEAR(iou(0, 0, 10, 10, 5, 0, 10, 10), 1.0f / 3.0f, 1e-5);
}

TEST(IouTest, DegenerateBoxIsZero) {
    EXPECT_FLOAT_EQ(iou(0, 0, 0, 10, 0, 0, 10, 10), 0.0f);
    EXPECT_FLOAT_EQ(iou(0, 0, 10, 10, 0, 0, 10, 0), 0.0f);
}

// Two overlapping detections across enough update() calls keep the SAME id.
TEST(TrackerTest, OverlappingDetectionsKeepSameId) {
    Tracker tr(0.3f, 30, /*min_hits=*/3);

    std::vector<Det> a = {makeDet(0, 0, 10, 10)};
    auto r1 = tr.update(a);   // hits=1 -> tentative, id 0
    EXPECT_EQ(r1[0], 0);

    std::vector<Det> b = {makeDet(1, 0, 10, 10)};
    auto r2 = tr.update(b);   // hits=2 -> still tentative
    EXPECT_EQ(r2[0], 0);

    std::vector<Det> c = {makeDet(2, 0, 10, 10)};
    auto r3 = tr.update(c);   // hits=3 -> confirmed, gets real id
    EXPECT_NE(r3[0], 0);

    std::vector<Det> d = {makeDet(1, 0, 10, 10)};
    auto r4 = tr.update(d);   // still matched -> same id
    EXPECT_EQ(r4[0], r3[0]);

    EXPECT_EQ(tr.track_count(), 1u);
}

// A track that disappears for > max_age frames is freed.
TEST(TrackerTest, StaleTrackIsRemoved) {
    Tracker tr(0.3f, /*max_age=*/2, /*min_hits=*/1);

    std::vector<Det> a = {makeDet(0, 0, 10, 10)};
    auto r1 = tr.update(a);   // min_hits=1 -> confirmed immediately
    EXPECT_NE(r1[0], 0);
    EXPECT_EQ(tr.track_count(), 1u);

    std::vector<Det> none;
    tr.update(none);  // time_since_update = 1 (<= max_age, survives)
    EXPECT_EQ(tr.track_count(), 1u);
    tr.update(none);  // time_since_update = 2 (<= max_age, survives)
    EXPECT_EQ(tr.track_count(), 1u);
    tr.update(none);  // time_since_update = 3 (> max_age, pruned)
    EXPECT_EQ(tr.track_count(), 0u);
}

// A new, non-overlapping detection gets a NEW id distinct from an existing track.
TEST(TrackerTest, NonOverlappingGetsNewId) {
    Tracker tr(0.3f, 30, /*min_hits=*/1);

    std::vector<Det> a = {makeDet(0, 0, 10, 10)};
    auto r1 = tr.update(a);
    const int firstId = r1[0];
    EXPECT_NE(firstId, 0);

    // Second batch: same first box plus a far-away box.
    std::vector<Det> b = {makeDet(0, 0, 10, 10), makeDet(500, 500, 10, 10)};
    auto r2 = tr.update(b);
    EXPECT_EQ(r2[0], firstId);   // first stays
    EXPECT_NE(r2[1], 0);         // new one confirmed (min_hits=1)
    EXPECT_NE(r2[1], firstId);   // and distinct
    EXPECT_EQ(tr.track_count(), 2u);
}

// Greedy association: each detection matches at most one track.
TEST(TrackerTest, OneToOneAssociation) {
    Tracker tr(0.3f, 30, /*min_hits=*/1);

    std::vector<Det> a = {makeDet(0, 0, 10, 10), makeDet(100, 0, 10, 10)};
    tr.update(a);
    EXPECT_EQ(tr.track_count(), 2u);

    std::vector<Det> b = {makeDet(1, 0, 10, 10), makeDet(101, 0, 10, 10)};
    tr.update(b);
    EXPECT_EQ(tr.track_count(), 2u);  // no spurious extra tracks
}
