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

// Class gating: a track must NOT re-acquire an overlapping detection of a
// different class. Otherwise a departed object's lingering track poaches a new
// object of another type, fusing two identities under one id.
TEST(TrackerTest, ClassGatedAssociationSplitsByClass) {
    Tracker tr(0.3f, /*max_age=*/30, /*min_hits=*/1, /*class_gated=*/true);

    // A "person" (class 0) occupies a box and is confirmed.
    std::vector<Det> a = {makeDet(0, 0, 10, 10, /*cls=*/0)};
    const int personId = tr.update(a)[0];
    EXPECT_NE(personId, 0);

    // Next frame a "car" (class 2) sits on the SAME box. With gating it cannot
    // inherit the person's id — it must spawn a new, distinct track.
    std::vector<Det> b = {makeDet(0, 0, 10, 10, /*cls=*/2)};
    const int carId = tr.update(b)[0];
    EXPECT_NE(carId, 0);
    EXPECT_NE(carId, personId);
    EXPECT_EQ(tr.track_count(), 2u);
}

// With gating OFF, the same overlapping cross-class detection re-uses the id
// (documents the old behaviour the gate prevents).
TEST(TrackerTest, UngatedAssociationAllowsCrossClassReuse) {
    Tracker tr(0.3f, /*max_age=*/30, /*min_hits=*/1, /*class_gated=*/false);

    const int id0 = tr.update({makeDet(0, 0, 10, 10, /*cls=*/0)})[0];
    const int id1 = tr.update({makeDet(0, 0, 10, 10, /*cls=*/2)})[0];
    EXPECT_EQ(id1, id0);              // same id across classes
    EXPECT_EQ(tr.track_count(), 1u);
}

// Appearance gate: two SAME-class overlapping detections that look DIFFERENT
// (orthogonal embeddings) must not share an id — this is the ReID fix for the
// "two cars under one id" case that class gating alone can't catch.
TEST(TrackerTest, AppearanceGateSplitsLookalikeOverlap) {
    Tracker tr(0.3f, /*max_age=*/30, /*min_hits=*/1, /*class_gated=*/true,
               /*appearance_threshold=*/0.5f, /*appearance_weight=*/0.3f);

    Det a = makeDet(0, 0, 10, 10, /*cls=*/2);
    a.embedding = {1.f, 0.f, 0.f};      // e.g. a white car
    const int id0 = tr.update({a})[0];
    EXPECT_NE(id0, 0);

    Det b = makeDet(0, 0, 10, 10, /*cls=*/2);
    b.embedding = {0.f, 1.f, 0.f};      // a different-looking car, same box+class
    const int id1 = tr.update({b})[0];
    EXPECT_NE(id1, 0);
    EXPECT_NE(id1, id0);                 // appearance mismatch -> distinct id
    EXPECT_EQ(tr.track_count(), 2u);
}

// Same class, same appearance, moving box -> stays one id (gate doesn't fragment
// a real track whose colour is stable frame to frame).
TEST(TrackerTest, AppearanceGateKeepsConsistentLook) {
    Tracker tr(0.3f, 30, /*min_hits=*/1, /*class_gated=*/true,
               /*appearance_threshold=*/0.5f, /*appearance_weight=*/0.3f);

    Det a = makeDet(0, 0, 10, 10, /*cls=*/2);
    a.embedding = {1.f, 0.f, 0.f};
    const int id0 = tr.update({a})[0];

    Det b = makeDet(1, 0, 10, 10, /*cls=*/2);
    b.embedding = {0.9f, 0.1f, 0.f};    // nearly identical look
    const int id1 = tr.update({b})[0];
    EXPECT_EQ(id1, id0);
    EXPECT_EQ(tr.track_count(), 1u);
}

// OC-SORT: a moving track survives a one-frame detection gap and re-acquires the
// same id (KF coast + recovery), instead of fragmenting into a new track.
TEST(TrackerTest, SurvivesGapKeepsId) {
    Tracker tr(0.3f, /*max_age=*/5, /*min_hits=*/1, /*class_gated=*/true,
               0.f, 0.f, 0.1f, /*det_high_thresh=*/0.5f, 0.2f, 0.2f);
    const int id = tr.update({makeDet(0, 0, 10, 10, 0, 0.9f)})[0];
    EXPECT_NE(id, 0);
    tr.update({makeDet(5, 0, 10, 10, 0, 0.9f)});    // build rightward velocity
    tr.update({makeDet(10, 0, 10, 10, 0, 0.9f)});
    tr.update({});                                   // gap: object missed one frame
    auto r = tr.update({makeDet(15, 0, 10, 10, 0, 0.9f)});  // reappears down-track
    EXPECT_EQ(r[0], id);            // recovered the same identity
    EXPECT_EQ(tr.track_count(), 1u);
}

// ByteTrack: a low-confidence detection recovers an existing track (second pass)
// but a lone low-confidence detection does NOT spawn a new track.
TEST(TrackerTest, ByteTrackLowConfRecoversButDoesNotSpawn) {
    Tracker tr(0.3f, /*max_age=*/5, /*min_hits=*/1, /*class_gated=*/true,
               0.f, 0.f, 0.1f, /*det_high_thresh=*/0.5f, 0.2f, 0.2f);
    const int id = tr.update({makeDet(0, 0, 10, 10, 0, 0.9f)})[0];  // high-conf -> track
    EXPECT_NE(id, 0);

    auto r1 = tr.update({makeDet(1, 0, 10, 10, 0, /*conf=*/0.3f)});  // low-conf overlap
    EXPECT_EQ(r1[0], id);                 // recovered (stage 2), same id
    EXPECT_EQ(tr.track_count(), 1u);

    auto r2 = tr.update({makeDet(500, 500, 10, 10, 0, /*conf=*/0.3f)});  // lone low-conf
    EXPECT_EQ(r2[0], 0);                   // no track
    EXPECT_EQ(tr.track_count(), 1u);      // and did not spawn
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
