// Unit tests for the pure top-k audio classifier helper.

#include "audio_topk.hpp"

#include <gtest/gtest.h>

using zm::audio::top_k_above_threshold;

TEST(AudioTopK, ReturnsIndicesInDescendingScoreOrder) {
    // scores:        0     1     2     3     4
    const float s[] = {0.10f, 0.90f, 0.30f, 0.70f, 0.50f};
    auto r = top_k_above_threshold(s, 5, /*thr=*/0.0f, /*k=*/5);
    ASSERT_EQ(r.size(), 5u);
    EXPECT_EQ(r[0].first, 1); // 0.90
    EXPECT_EQ(r[1].first, 3); // 0.70
    EXPECT_EQ(r[2].first, 4); // 0.50
    EXPECT_EQ(r[3].first, 2); // 0.30
    EXPECT_EQ(r[4].first, 0); // 0.10
    EXPECT_FLOAT_EQ(r[0].second, 0.90f);
}

TEST(AudioTopK, ExcludesEntriesBelowThreshold) {
    const float s[] = {0.10f, 0.90f, 0.30f, 0.70f, 0.50f};
    auto r = top_k_above_threshold(s, 5, /*thr=*/0.5f, /*k=*/10);
    // Only 0.90, 0.70, 0.50 are >= 0.5.
    ASSERT_EQ(r.size(), 3u);
    EXPECT_EQ(r[0].first, 1);
    EXPECT_EQ(r[1].first, 3);
    EXPECT_EQ(r[2].first, 4);
    EXPECT_FLOAT_EQ(r[2].second, 0.50f);
}

TEST(AudioTopK, CapsTheCount) {
    const float s[] = {0.10f, 0.90f, 0.30f, 0.70f, 0.50f};
    auto r = top_k_above_threshold(s, 5, /*thr=*/0.0f, /*k=*/2);
    ASSERT_EQ(r.size(), 2u);
    EXPECT_EQ(r[0].first, 1); // 0.90
    EXPECT_EQ(r[1].first, 3); // 0.70
}

TEST(AudioTopK, EmptyWhenNothingPassesThreshold) {
    const float s[] = {0.10f, 0.20f, 0.30f};
    auto r = top_k_above_threshold(s, 3, /*thr=*/0.9f, /*k=*/3);
    EXPECT_TRUE(r.empty());
}

TEST(AudioTopK, HandlesNullAndZeroAndNonPositiveK) {
    const float s[] = {0.5f};
    EXPECT_TRUE(top_k_above_threshold(nullptr, 5, 0.0f, 3).empty());
    EXPECT_TRUE(top_k_above_threshold(s, 0, 0.0f, 3).empty());
    EXPECT_TRUE(top_k_above_threshold(s, 1, 0.0f, 0).empty());
    EXPECT_TRUE(top_k_above_threshold(s, 1, 0.0f, -1).empty());
}

TEST(AudioTopK, TieBreaksByAscendingIndex) {
    const float s[] = {0.5f, 0.5f, 0.5f};
    auto r = top_k_above_threshold(s, 3, /*thr=*/0.0f, /*k=*/3);
    ASSERT_EQ(r.size(), 3u);
    EXPECT_EQ(r[0].first, 0);
    EXPECT_EQ(r[1].first, 1);
    EXPECT_EQ(r[2].first, 2);
}
