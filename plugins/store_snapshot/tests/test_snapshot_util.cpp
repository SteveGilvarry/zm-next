// Unit tests for the pure should_snapshot() helper: type filter + throttle.

#include "snapshot_util.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using zm::storesnapshot::should_snapshot;

namespace {

const std::vector<std::string> kEmpty{};

// --- Type filter --------------------------------------------------------------

TEST(SnapshotUtil, EmptyTriggerUsesDefaultSet) {
    // Each default type triggers (throttle satisfied: last=0, interval=2000).
    for (const char* t : {"detection", "motion", "face", "lpr", "analytics",
                          "audio_event"}) {
        EXPECT_TRUE(should_snapshot(kEmpty, t, /*now*/ 10000, /*last*/ 0, 2000))
            << "type=" << t;
    }
}

TEST(SnapshotUtil, EmptyTriggerRejectsUnknownType) {
    EXPECT_FALSE(should_snapshot(kEmpty, "heartbeat", 10000, 0, 2000));
    EXPECT_FALSE(should_snapshot(kEmpty, "description", 10000, 0, 2000));
}

TEST(SnapshotUtil, EmptyTypeNeverTriggers) {
    EXPECT_FALSE(should_snapshot(kEmpty, "", 10000, 0, 2000));
    std::vector<std::string> all{"motion"};
    EXPECT_FALSE(should_snapshot(all, "", 10000, 0, 2000));
}

TEST(SnapshotUtil, ExplicitListMatchesOnlyConfigured) {
    std::vector<std::string> cfg{"motion", "lpr"};
    EXPECT_TRUE(should_snapshot(cfg, "motion", 10000, 0, 2000));
    EXPECT_TRUE(should_snapshot(cfg, "lpr", 10000, 0, 2000));
    // "detection" is a default but NOT in this explicit list -> rejected.
    EXPECT_FALSE(should_snapshot(cfg, "detection", 10000, 0, 2000));
}

TEST(SnapshotUtil, ExplicitNoneDisablesAll) {
    std::vector<std::string> cfg{"none"};
    EXPECT_FALSE(should_snapshot(cfg, "motion", 10000, 0, 2000));
    EXPECT_FALSE(should_snapshot(cfg, "detection", 10000, 0, 2000));
}

// --- Throttle window ----------------------------------------------------------

TEST(SnapshotUtil, ThrottleBlocksWithinWindow) {
    // last snapshot at 1000ms, interval 2000ms.
    EXPECT_FALSE(should_snapshot(kEmpty, "motion", /*now*/ 1500, /*last*/ 1000,
                                 2000));
    EXPECT_FALSE(should_snapshot(kEmpty, "motion", /*now*/ 2999, /*last*/ 1000,
                                 2000));
}

TEST(SnapshotUtil, ThrottleAllowsAtBoundary) {
    // Exactly min_interval_ms elapsed -> allowed (>=).
    EXPECT_TRUE(should_snapshot(kEmpty, "motion", /*now*/ 3000, /*last*/ 1000,
                                2000));
}

TEST(SnapshotUtil, ThrottleAllowsAfterWindow) {
    EXPECT_TRUE(should_snapshot(kEmpty, "motion", /*now*/ 5000, /*last*/ 1000,
                                2000));
}

TEST(SnapshotUtil, ZeroIntervalAlwaysAllows) {
    EXPECT_TRUE(should_snapshot(kEmpty, "motion", 1000, 1000, 0));
    EXPECT_TRUE(should_snapshot(kEmpty, "motion", 1000, 1000, -5));
}

TEST(SnapshotUtil, ThrottleDoesNotBypassTypeFilter) {
    // Even with the window satisfied, a non-trigger type stays rejected.
    EXPECT_FALSE(should_snapshot(kEmpty, "heartbeat", 100000, 0, 2000));
}

}  // namespace
