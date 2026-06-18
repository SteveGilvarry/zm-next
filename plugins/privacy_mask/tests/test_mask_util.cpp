#include "../mask_util.hpp"

#include <gtest/gtest.h>
#include <vector>
#include <cstdint>

using namespace zm::privacy;

namespace {
// A 10x10 axis-aligned square covering [2,8) x [2,8) (pixel centers 2.5..7.5).
std::vector<Pt> rectPoly() {
    return {{2.f, 2.f}, {8.f, 2.f}, {8.f, 8.f}, {2.f, 8.f}};
}
}  // namespace

TEST(PointInPolygon, InsideRectangle) {
    auto poly = rectPoly();
    EXPECT_TRUE(point_in_polygon(poly, 5.f, 5.f));
    EXPECT_TRUE(point_in_polygon(poly, 2.5f, 2.5f));
    EXPECT_TRUE(point_in_polygon(poly, 7.5f, 7.5f));
}

TEST(PointInPolygon, OutsideRectangle) {
    auto poly = rectPoly();
    EXPECT_FALSE(point_in_polygon(poly, 0.f, 0.f));
    EXPECT_FALSE(point_in_polygon(poly, 9.f, 9.f));
    EXPECT_FALSE(point_in_polygon(poly, 5.f, 0.f));   // above
    EXPECT_FALSE(point_in_polygon(poly, 0.f, 5.f));   // left
}

TEST(PointInPolygon, Triangle) {
    // Triangle (0,0)-(10,0)-(0,10): inside x+y<10 (roughly).
    std::vector<Pt> tri = {{0.f, 0.f}, {10.f, 0.f}, {0.f, 10.f}};
    EXPECT_TRUE(point_in_polygon(tri, 1.f, 1.f));
    EXPECT_TRUE(point_in_polygon(tri, 2.f, 3.f));
    EXPECT_FALSE(point_in_polygon(tri, 8.f, 8.f));   // beyond hypotenuse
    EXPECT_FALSE(point_in_polygon(tri, -1.f, 5.f));  // outside left
}

TEST(PointInPolygon, DegeneratePolygon) {
    std::vector<Pt> two = {{0.f, 0.f}, {5.f, 5.f}};
    EXPECT_FALSE(point_in_polygon(two, 1.f, 1.f));   // <3 verts -> never inside
    std::vector<Pt> empty;
    EXPECT_FALSE(point_in_polygon(empty, 0.f, 0.f));
}

TEST(BlackRegion, ZerosInsideLeavesOutside) {
    const int w = 10, h = 10, ch = 3;
    std::vector<uint8_t> img(static_cast<size_t>(w) * h * ch, 200);  // gray
    auto poly = rectPoly();
    black_region(img.data(), w, h, ch, poly);

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const uint8_t* p = img.data() + (static_cast<size_t>(y) * w + x) * ch;
            const bool inside = point_in_polygon(poly,
                static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f);
            if (inside) {
                EXPECT_EQ(p[0], 0) << "x=" << x << " y=" << y;
                EXPECT_EQ(p[1], 0);
                EXPECT_EQ(p[2], 0);
            } else {
                EXPECT_EQ(p[0], 200) << "x=" << x << " y=" << y;
                EXPECT_EQ(p[1], 200);
                EXPECT_EQ(p[2], 200);
            }
        }
    }
}

TEST(BlackRegion, GrayscaleChannel) {
    const int w = 10, h = 10, ch = 1;
    std::vector<uint8_t> img(static_cast<size_t>(w) * h * ch, 150);
    auto poly = rectPoly();
    black_region(img.data(), w, h, ch, poly);

    // A clearly-inside pixel and a clearly-outside pixel.
    EXPECT_EQ(img[5 * w + 5], 0);     // (5,5) inside
    EXPECT_EQ(img[0 * w + 0], 150);   // (0,0) outside
}

TEST(PixelateRegion, AveragesInsideOnly) {
    // 8x8 RGB image, left half value 0, right half value 100. Pixelate a full
    // -image polygon with one big block; inside pixels become the average,
    // and at least it must NOT touch any pixel outside the polygon.
    const int w = 8, h = 8, ch = 3;
    std::vector<uint8_t> img(static_cast<size_t>(w) * h * ch);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t v = (x < 4) ? 0 : 100;
            uint8_t* p = img.data() + (static_cast<size_t>(y) * w + x) * ch;
            p[0] = p[1] = p[2] = v;
        }
    // Polygon covering only the right half [4,8) x [0,8).
    std::vector<Pt> poly = {{4.f, 0.f}, {8.f, 0.f}, {8.f, 8.f}, {4.f, 8.f}};
    pixelate_region(img.data(), w, h, ch, poly, 16);  // one block spanning bbox

    // Left half (outside polygon) untouched.
    EXPECT_EQ(img[(0 * w + 0) * ch], 0);
    EXPECT_EQ(img[(3 * w + 0) * ch + 0], 0);
    // Right half (inside) all become the average of the inside pixels (=100).
    EXPECT_EQ(img[(0 * w + 5) * ch], 100);
    EXPECT_EQ(img[(7 * w + 7) * ch], 100);
}
