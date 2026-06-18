#include "../motion_diff.hpp"

#include <gtest/gtest.h>
#include <vector>
#include <cstdint>

using namespace zm::motiongate;

TEST(MotionDiffTest, DownsampleGrayPicksEveryNthPixel) {
    // 4x2 gray ramp 0..7; step 2 -> samples at x=0,2 ; y=0 -> {0,2}, y... step 2 only y=0.
    std::vector<uint8_t> img = {0, 1, 2, 3, 4, 5, 6, 7};
    std::vector<uint8_t> out;
    int dw = 0, dh = 0;
    downsample_luma(img.data(), PixFmt::GRAY8, 4, 2, 2, out, dw, dh);
    EXPECT_EQ(dw, 2);
    EXPECT_EQ(dh, 1);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0], 0);  // (0,0)
    EXPECT_EQ(out[1], 2);  // (2,0)
}

TEST(MotionDiffTest, RgbLumaWeighted) {
    // One white pixel -> luma ~255; one black -> 0.
    std::vector<uint8_t> img = {255, 255, 255, 0, 0, 0};
    std::vector<uint8_t> out;
    int dw = 0, dh = 0;
    downsample_luma(img.data(), PixFmt::RGB24, 2, 1, 1, out, dw, dh);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_GE(out[0], 250);   // white
    EXPECT_EQ(out[1], 0);     // black
}

TEST(MotionDiffTest, CountChangedThresholds) {
    std::vector<uint8_t> a = {10, 10, 10, 10};
    std::vector<uint8_t> b = {10, 40, 12, 100};  // deltas 0,30,2,90
    // threshold 20 -> only deltas >20 count: 30 and 90 -> 2
    EXPECT_EQ(count_changed(a, b, 20), 2);
    // threshold 100 -> none
    EXPECT_EQ(count_changed(a, b, 100), 0);
    // size mismatch -> -1
    std::vector<uint8_t> c = {1, 2, 3};
    EXPECT_EQ(count_changed(a, c, 5), -1);
}

TEST(MotionDiffTest, IdenticalFramesNoChange) {
    std::vector<uint8_t> a(100, 128);
    std::vector<uint8_t> b(100, 128);
    EXPECT_EQ(count_changed(a, b, 0), 0);
}
