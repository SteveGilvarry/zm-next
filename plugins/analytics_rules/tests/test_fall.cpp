#include "fall.hpp"

#include <gtest/gtest.h>

using namespace zm::analytics::fall;

namespace {

constexpr uint64_t kSec = 1000000ull;

Keypoint kp(float x, float y, float v = 0.9f) { return {x, y, v}; }

// All 17 keypoints present but invisible; tests fill in the ones they use.
Pose blank(float x, float y, float w, float h) {
    Pose p;
    p.x = x; p.y = y; p.w = w; p.h = h;
    p.kpts.assign(KpCount, Keypoint{0, 0, 0});
    return p;
}

// Standing person, 60x180 box at (cx-30, top): head on top, hips mid, upright torso.
Pose standing(float cx = 500, float top = 200) {
    Pose p = blank(cx - 30, top, 60, 180);
    p.kpts[Nose] = kp(cx, top + 15);
    p.kpts[LEye] = kp(cx - 5, top + 10);
    p.kpts[REye] = kp(cx + 5, top + 10);
    p.kpts[LShoulder] = kp(cx - 20, top + 45);
    p.kpts[RShoulder] = kp(cx + 20, top + 45);
    p.kpts[LHip] = kp(cx - 15, top + 100);
    p.kpts[RHip] = kp(cx + 15, top + 100);
    return p;
}

// Lying on the floor, head to the left: 180x60 box, torso horizontal, head at hip level.
Pose lying(float cx = 500, float cy = 400) {
    Pose p = blank(cx - 90, cy - 30, 180, 60);
    p.kpts[Nose] = kp(cx - 80, cy + 2);
    p.kpts[LShoulder] = kp(cx - 50, cy - 10);
    p.kpts[RShoulder] = kp(cx - 50, cy + 10);
    p.kpts[LHip] = kp(cx + 10, cy - 8);
    p.kpts[RHip] = kp(cx + 10, cy + 8);
    return p;
}

// Sitting: box nearly square, but torso upright and head above hips.
Pose sitting(float cx = 500, float top = 250) {
    Pose p = blank(cx - 55, top, 110, 115);
    p.kpts[Nose] = kp(cx, top + 12);
    p.kpts[LShoulder] = kp(cx - 20, top + 40);
    p.kpts[RShoulder] = kp(cx + 20, top + 40);
    p.kpts[LHip] = kp(cx - 15, top + 95);
    p.kpts[RHip] = kp(cx + 15, top + 95);
    return p;
}

// Bending to tie a shoe: torso tilted forward but box taller than wide, head above hips.
Pose bending(float cx = 500, float top = 250) {
    Pose p = blank(cx - 40, top, 80, 140);
    p.kpts[Nose] = kp(cx + 35, top + 55);
    p.kpts[LShoulder] = kp(cx + 20, top + 50);
    p.kpts[RShoulder] = kp(cx + 25, top + 55);
    p.kpts[LHip] = kp(cx - 20, top + 70);
    p.kpts[RHip] = kp(cx - 15, top + 72);
    return p;
}

// Feed `pose` every 100 ms from t0 for `secs`; return the first fire seen.
Fire feed(Person& st, const Pose& pose, uint64_t& t, double secs, const Params& prm) {
    Fire first;
    const uint64_t end = t + static_cast<uint64_t>(secs * kSec);
    for (; t < end; t += kSec / 10) {
        const Fire f = step(st, pose, t, prm);
        if (f.fire && !first.fire) first = f;
    }
    return first;
}

}  // namespace

// ---------------------------------------------------------------------------
// posture signals
// ---------------------------------------------------------------------------
TEST(FallPosture, StandingIsUpright) {
    const Signals s = posture(standing(), Params{});
    EXPECT_EQ(s.computed, 3);
    EXPECT_EQ(s.agree, 0);
    EXPECT_LT(s.tilt_deg, 10.0f);
    EXPECT_TRUE(is_upright(s));
    EXPECT_FALSE(is_down(s, Params{}));
}

TEST(FallPosture, LyingIsDownOnAllThree) {
    const Signals s = posture(lying(), Params{});
    EXPECT_EQ(s.computed, 3);
    EXPECT_TRUE(s.tilt);
    EXPECT_TRUE(s.wide);
    EXPECT_TRUE(s.head_low);
    EXPECT_GT(s.tilt_deg, 80.0f);
    EXPECT_TRUE(is_down(s, Params{}));
}

TEST(FallPosture, SittingIsNotDown) {
    // The box is square-ish (w/h 0.96) so "wide" alone might trip a looser aspect;
    // torso and head disagree, so it is never down.
    Params prm;
    prm.aspect = 0.9f;
    const Signals s = posture(sitting(), prm);
    EXPECT_TRUE(s.wide);
    EXPECT_FALSE(s.tilt);
    EXPECT_FALSE(s.head_low);
    EXPECT_FALSE(is_down(s, prm));
}

TEST(FallPosture, BendingIsNotDown) {
    const Signals s = posture(bending(), Params{});
    EXPECT_TRUE(s.tilt);          // torso is well forward
    EXPECT_FALSE(s.wide);
    EXPECT_FALSE(s.head_low);
    EXPECT_FALSE(is_down(s, Params{}));
}

TEST(FallPosture, LowConfidenceKeypointsAreIgnored) {
    Pose p = lying();
    for (auto& k : p.kpts) k.v = 0.2f;      // pose model unsure of every joint
    const Signals s = posture(p, Params{});
    EXPECT_EQ(s.computed, 1);               // only the bbox aspect is usable
    // One signal computed, and it agrees: min(2, 1) = 1 needed.
    EXPECT_TRUE(is_down(s, Params{}));
}

TEST(FallPosture, NoKeypointsNoBoxIsNeither) {
    Pose p = blank(0, 0, 0, 0);
    const Signals s = posture(p, Params{});
    EXPECT_EQ(s.computed, 0);
    EXPECT_FALSE(is_down(s, Params{}));
    EXPECT_FALSE(is_upright(s));
}

TEST(FallPosture, HeadFallsBackToEyesWhenNoseMissing) {
    Pose p = lying();
    p.kpts[Nose].v = 0;
    const float midHipY = (p.kpts[LHip].y + p.kpts[RHip].y) / 2;
    p.kpts[LEye] = kp(p.kpts[Nose].x, midHipY + 5);   // only the left eye visible
    const Signals s = posture(p, Params{});
    EXPECT_TRUE(s.head_low);
}

// ---------------------------------------------------------------------------
// step(): the episode state machine
// ---------------------------------------------------------------------------
TEST(FallStep, StandThenFallFiresAfterSeconds) {
    Params prm;               // 2 s down, require upright within 5 s
    Person st;
    uint64_t t = 0;
    EXPECT_FALSE(feed(st, standing(), t, 3.0, prm).fire);
    const uint64_t fellAt = t;
    Fire f = feed(st, lying(), t, 1.5, prm);
    EXPECT_FALSE(f.fire) << "fired before `seconds` elapsed";
    f = feed(st, lying(), t, 1.0, prm);
    ASSERT_TRUE(f.fire);
    EXPECT_GE(f.down_sec, 2.0);
    EXPECT_LT(f.down_sec, 2.2);
    EXPECT_GE(t - fellAt, 2 * kSec);
}

TEST(FallStep, FiresOncePerEpisodeAndRearmsAfterStandingUp) {
    Params prm;
    Person st;
    uint64_t t = 0;
    feed(st, standing(), t, 1.0, prm);
    EXPECT_TRUE(feed(st, lying(), t, 3.0, prm).fire);
    EXPECT_FALSE(feed(st, lying(), t, 10.0, prm).fire) << "second fire in one episode";
    feed(st, standing(), t, 1.0, prm);
    EXPECT_TRUE(feed(st, lying(), t, 3.0, prm).fire) << "did not re-arm";
}

TEST(FallStep, AlreadyLyingWhenCameraStartsDoesNotFire) {
    Params prm;
    Person st;
    uint64_t t = 0;
    EXPECT_FALSE(feed(st, lying(), t, 30.0, prm).fire);
}

TEST(FallStep, AlreadyLyingFiresWhenTransitionNotRequired) {
    Params prm;
    prm.require_upright = false;
    Person st;
    uint64_t t = 0;
    EXPECT_TRUE(feed(st, lying(), t, 3.0, prm).fire);
}

TEST(FallStep, UprightTooLongAgoDoesNotCount) {
    Params prm;
    prm.upright_window_sec = 5.0;
    Person st;
    uint64_t t = 0;
    feed(st, standing(), t, 1.0, prm);
    // 8 s of ambiguous frames (no keypoints, no box) between standing and lying.
    feed(st, blank(0, 0, 0, 0), t, 8.0, prm);
    EXPECT_FALSE(feed(st, lying(), t, 5.0, prm).fire);
}

TEST(FallStep, CrawlingAlongTheFloorDoesNotFire) {
    Params prm;
    Person st;
    uint64_t t = 0;
    feed(st, standing(), t, 1.0, prm);
    // Down, but the body moves a full box-height (180 px) every 0.5 s.
    bool fired = false;
    for (int i = 0; i < 60; ++i, t += kSec / 10) {
        const Pose p = lying(500 + i * 36.0f, 400);
        fired |= step(st, p, t, prm).fire;
    }
    EXPECT_FALSE(fired);
}

TEST(FallStep, SittingDownDoesNotFire) {
    Params prm;
    Person st;
    uint64_t t = 0;
    feed(st, standing(), t, 2.0, prm);
    EXPECT_FALSE(feed(st, sitting(), t, 30.0, prm).fire);
}

TEST(FallStep, BendingOverDoesNotFire) {
    Params prm;
    Person st;
    uint64_t t = 0;
    feed(st, standing(), t, 2.0, prm);
    EXPECT_FALSE(feed(st, bending(), t, 30.0, prm).fire);
}

// ---------------------------------------------------------------------------
// associate(): following people between frames
// ---------------------------------------------------------------------------
TEST(FallAssociate, KeepsIdsForTwoPeopleWhoMoveALittle) {
    std::vector<Person> people;
    int nextId = 1;
    auto a = associate(people, {standing(300), standing(900)}, 0, nextId);
    ASSERT_EQ(people.size(), 2u);
    const int idLeft = people[a[0]].id, idRight = people[a[1]].id;

    // Next frame: both moved 20 px, and the order in the event is swapped.
    auto b = associate(people, {standing(920), standing(320)}, kSec / 10, nextId);
    EXPECT_EQ(people.size(), 2u);
    EXPECT_EQ(people[b[0]].id, idRight);
    EXPECT_EQ(people[b[1]].id, idLeft);
}

TEST(FallAssociate, FarAwayPoseIsANewPerson) {
    std::vector<Person> people;
    int nextId = 1;
    associate(people, {standing(300)}, 0, nextId);
    auto b = associate(people, {standing(1500)}, kSec / 10, nextId);
    EXPECT_EQ(people.size(), 2u);
    EXPECT_EQ(people[b[0]].id, 2);
}

TEST(FallAssociate, StalePeopleAreDropped) {
    std::vector<Person> people;
    int nextId = 1;
    auto a = associate(people, {standing(300)}, 0, nextId);
    people[a[0]].last_seen = 0;
    associate(people, {standing(1500)}, 11 * kSec, nextId);
    ASSERT_EQ(people.size(), 1u);
    EXPECT_EQ(people[0].id, 2);
}

TEST(FallAssociate, StandingToLyingInPlaceKeepsTheSamePerson) {
    // The centre drops from mid-body to floor level as the person falls; that is
    // about 0.6 box-heights for this geometry, inside the default max_dist of 1.
    std::vector<Person> people;
    int nextId = 1;
    auto a = associate(people, {standing(500, 250)}, 0, nextId);
    people[a[0]].last_seen = 0;
    auto b = associate(people, {lying(500, 400)}, kSec / 10, nextId);
    EXPECT_EQ(people.size(), 1u);
    EXPECT_EQ(people[b[0]].id, 1);
}
