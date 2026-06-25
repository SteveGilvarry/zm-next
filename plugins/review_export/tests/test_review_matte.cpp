// Unit tests for the pure matte helpers used by review_export (motion synopsis):
// polygon rasterisation, feather, and premultiplication. No FFmpeg/model needed.

#include "review_matte.hpp"

#include <gtest/gtest.h>

#include <array>
#include <vector>

using zm::review::fill_polygon;
using zm::review::box_blur3;
using zm::review::premultiply_rgb;

namespace {
std::vector<std::array<float, 2>> square(float x0, float y0, float x1, float y1) {
    return {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
}
}  // namespace

TEST(ReviewMatte, EmptyPolygonFillsAll) {
    std::vector<uint8_t> mask;
    fill_polygon(mask, 4, 4, {}, 0, 0);
    ASSERT_EQ(mask.size(), 16u);
    for (uint8_t v : mask) EXPECT_EQ(v, 255);
}

TEST(ReviewMatte, FullCoverSquareIsAllInside) {
    // A 10x10 crop at origin, polygon covering it entirely.
    std::vector<uint8_t> mask;
    fill_polygon(mask, 10, 10, square(0, 0, 10, 10), 0, 0);
    int inside = 0;
    for (uint8_t v : mask) inside += (v == 255);
    EXPECT_EQ(inside, 100);
}

TEST(ReviewMatte, PolygonRasterisesInteriorAndExterior) {
    // 20x20 crop; a centered 10x10 square (5..15). Corners outside, center inside.
    std::vector<uint8_t> mask;
    fill_polygon(mask, 20, 20, square(5, 5, 15, 15), 0, 0);
    auto at = [&](int x, int y) { return mask[static_cast<size_t>(y) * 20 + x]; };
    EXPECT_EQ(at(10, 10), 255);   // center inside
    EXPECT_EQ(at(0, 0), 0);       // corner outside
    EXPECT_EQ(at(19, 19), 0);     // corner outside
    int inside = 0;
    for (uint8_t v : mask) inside += (v == 255);
    EXPECT_GT(inside, 60);        // ~100 px filled (allow rounding slack)
    EXPECT_LT(inside, 140);
}

TEST(ReviewMatte, CropOriginOffsetTranslatesPolygon) {
    // Source polygon at (105..115); crop origin (100,100) -> local (5..15) in a 20x20.
    std::vector<uint8_t> mask;
    fill_polygon(mask, 20, 20, square(105, 105, 115, 115), 100, 100);
    auto at = [&](int x, int y) { return mask[static_cast<size_t>(y) * 20 + x]; };
    EXPECT_EQ(at(10, 10), 255);
    EXPECT_EQ(at(0, 0), 0);
}

TEST(ReviewMatte, BoxBlurSoftensEdge) {
    // Hard half-filled mask; after blur the boundary column is no longer pure 0/255.
    const int w = 9, h = 9;
    std::vector<uint8_t> mask(static_cast<size_t>(w) * h, 0);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            if (x >= w / 2) mask[static_cast<size_t>(y) * w + x] = 255;
    box_blur3(mask, w, h);
    // A pixel straddling the boundary should be an intermediate value.
    const uint8_t v = mask[static_cast<size_t>(4) * w + (w / 2)];
    EXPECT_GT(v, 0);
    EXPECT_LT(v, 255);
}

TEST(ReviewMatte, PremultiplyZeroesBackgroundKeepsForeground) {
    // 2 pixels: one masked-out (0), one fully kept (255).
    std::vector<uint8_t> rgb = {200, 100, 50,  10, 20, 30};
    std::vector<uint8_t> mask = {0, 255};
    premultiply_rgb(rgb, mask);
    EXPECT_EQ(rgb[0], 0);  EXPECT_EQ(rgb[1], 0);  EXPECT_EQ(rgb[2], 0);   // background -> black
    EXPECT_EQ(rgb[3], 10); EXPECT_EQ(rgb[4], 20); EXPECT_EQ(rgb[5], 30);  // foreground intact
}

TEST(ReviewMatte, PremultiplyHalfMatteScales) {
    std::vector<uint8_t> rgb = {200, 200, 200};
    std::vector<uint8_t> mask = {128};
    premultiply_rgb(rgb, mask);
    EXPECT_NEAR(rgb[0], 100, 2);
}
