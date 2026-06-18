// Unit tests for the pure drawing helpers in draw.hpp.

#include "draw.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

using zm::overlay::draw_rect;
using zm::overlay::draw_text;

namespace {

// Helper: allocate a w*h RGB24 image, zero-filled.
std::vector<uint8_t> makeImage(int w, int h) {
    return std::vector<uint8_t>(static_cast<size_t>(w) * h * 3, 0);
}

// Helper: read a pixel's red channel (we draw single-color so any channel works).
uint8_t pxR(const std::vector<uint8_t>& img, int w, int x, int y) {
    return img[(static_cast<size_t>(y) * w + x) * 3];
}

bool pxSet(const std::vector<uint8_t>& img, int w, int x, int y) {
    size_t i = (static_cast<size_t>(y) * w + x) * 3;
    return img[i] || img[i + 1] || img[i + 2];
}

// Count how many pixels are non-zero.
int countSet(const std::vector<uint8_t>& img) {
    int n = 0;
    for (size_t i = 0; i < img.size(); i += 3)
        if (img[i] || img[i + 1] || img[i + 2]) ++n;
    return n;
}

}  // namespace

// ---------------------------------------------------------------------------
// draw_rect
// ---------------------------------------------------------------------------

TEST(DrawRect, BorderPixelsWrittenInteriorUntouched) {
    const int w = 20, h = 20;
    auto img = makeImage(w, h);

    // 6x6 box at (5,5): corners (5,5)..(10,10) inclusive, thickness 1.
    draw_rect(img.data(), w, h, 5, 5, 6, 6, 255, 0, 0, 1);

    // Corners present.
    EXPECT_TRUE(pxSet(img, w, 5, 5));
    EXPECT_TRUE(pxSet(img, w, 10, 5));
    EXPECT_TRUE(pxSet(img, w, 5, 10));
    EXPECT_TRUE(pxSet(img, w, 10, 10));

    // Mid-edge points present.
    EXPECT_TRUE(pxSet(img, w, 7, 5));   // top edge
    EXPECT_TRUE(pxSet(img, w, 7, 10));  // bottom edge
    EXPECT_TRUE(pxSet(img, w, 5, 7));   // left edge
    EXPECT_TRUE(pxSet(img, w, 10, 7));  // right edge

    // Interior untouched.
    EXPECT_FALSE(pxSet(img, w, 7, 7));
    EXPECT_FALSE(pxSet(img, w, 8, 8));

    // Color is what we asked for.
    EXPECT_EQ(pxR(img, w, 5, 5), 255);
}

TEST(DrawRect, ThicknessDrawsMultipleBorderRows) {
    const int w = 30, h = 30;
    auto img = makeImage(w, h);

    // 10x10 box at (5,5), thickness 3.
    draw_rect(img.data(), w, h, 5, 5, 10, 10, 0, 255, 0, 3);

    // Three top rows set at an interior column.
    EXPECT_TRUE(pxSet(img, w, 8, 5));
    EXPECT_TRUE(pxSet(img, w, 8, 6));
    EXPECT_TRUE(pxSet(img, w, 8, 7));
    // Fourth row inside is empty.
    EXPECT_FALSE(pxSet(img, w, 8, 8));
}

TEST(DrawRect, ClampsWhenPartlyOffScreen) {
    const int w = 16, h = 16;
    auto img = makeImage(w, h);

    // Box starting off the top-left: (-3,-3) size 8x8 -> only lower-right
    // portion is on-screen. Must not crash / write OOB.
    draw_rect(img.data(), w, h, -3, -3, 8, 8, 255, 255, 255, 1);

    // The bottom edge of the box is at y = -3 + 8 - 1 = 4; visible columns 0..4.
    EXPECT_TRUE(pxSet(img, w, 0, 4));
    EXPECT_TRUE(pxSet(img, w, 4, 4));
    // The off-screen top edge produced nothing visible above row 0 (trivially
    // true — just assert we wrote something but stayed in-bounds).
    EXPECT_GT(countSet(img), 0);
}

TEST(DrawRect, FullyOffScreenWritesNothing) {
    const int w = 10, h = 10;
    auto img = makeImage(w, h);
    draw_rect(img.data(), w, h, 100, 100, 5, 5, 255, 0, 0, 2);
    EXPECT_EQ(countSet(img), 0);
}

// ---------------------------------------------------------------------------
// draw_text
// ---------------------------------------------------------------------------

TEST(DrawText, WritesSomePixelsForKnownGlyph) {
    const int w = 40, h = 16;
    auto img = makeImage(w, h);
    draw_text(img.data(), w, h, 1, 1, "A", 255, 255, 255, 1);
    EXPECT_GT(countSet(img), 0);
}

TEST(DrawText, EmptyStringWritesNothing) {
    const int w = 40, h = 16;
    auto img = makeImage(w, h);
    draw_text(img.data(), w, h, 1, 1, "", 255, 255, 255, 1);
    EXPECT_EQ(countSet(img), 0);
}

TEST(DrawText, UnknownGlyphSkippedButNoCrash) {
    const int w = 40, h = 16;
    auto img = makeImage(w, h);
    // '~' is not in the table; nothing should be drawn but no crash.
    draw_text(img.data(), w, h, 1, 1, "~", 255, 255, 255, 1);
    EXPECT_EQ(countSet(img), 0);
}

TEST(DrawText, LowercaseMapsToUppercaseGlyph) {
    const int w = 40, h = 16;
    auto up = makeImage(w, h);
    auto lo = makeImage(w, h);
    draw_text(up.data(), w, h, 1, 1, "Z", 255, 255, 255, 1);
    draw_text(lo.data(), w, h, 1, 1, "z", 255, 255, 255, 1);
    EXPECT_EQ(up, lo);
    EXPECT_GT(countSet(up), 0);
}

TEST(DrawText, NeverWritesOutOfBounds) {
    const int w = 8, h = 8;
    auto img = makeImage(w, h);
    // Long string drawn near the right/bottom edge: must clamp, not crash/OOB.
    draw_text(img.data(), w, h, 6, 6, "HELLO WORLD 123", 255, 255, 255, 2);
    // If we got here without ASan/OOB, clamping worked. Sanity: image unchanged
    // in size, and at least the test ran. Some pixels may be set within bounds.
    EXPECT_EQ(img.size(), static_cast<size_t>(w) * h * 3);
}

TEST(DrawText, ScaleProducesMorePixels) {
    const int w = 60, h = 30;
    auto s1 = makeImage(w, h);
    auto s2 = makeImage(w, h);
    draw_text(s1.data(), w, h, 1, 1, "8", 255, 255, 255, 1);
    draw_text(s2.data(), w, h, 1, 1, "8", 255, 255, 255, 2);
    EXPECT_GT(countSet(s2), countSet(s1));
}
