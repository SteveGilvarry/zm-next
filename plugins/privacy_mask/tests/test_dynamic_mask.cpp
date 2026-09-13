#include "dynamic_mask.hpp"

#include <gtest/gtest.h>

#include <numeric>
#include <random>

using namespace zm::privacy;

namespace {

std::vector<uint8_t> noise(int w, int h, int ch, unsigned seed = 7) {
    std::vector<uint8_t> v(static_cast<size_t>(w) * h * ch);
    std::mt19937 rng(seed);
    for (auto& b : v) b = static_cast<uint8_t>(rng() & 0xff);
    return v;
}

uint8_t at(const std::vector<uint8_t>& v, int w, int ch, int x, int y, int c) {
    return v[(static_cast<size_t>(y) * w + x) * ch + c];
}

bool outsideUnchanged(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b,
                      int w, int h, int ch, const Rect& r) {
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            if (x >= r.x0 && x < r.x1 && y >= r.y0 && y < r.y1) continue;
            for (int c = 0; c < ch; ++c)
                if (at(a, w, ch, x, y, c) != at(b, w, ch, x, y, c)) return false;
        }
    return true;
}

// Naive clamped box blur of one rect, one horizontal then one vertical pass.
std::vector<uint8_t> referenceBlur(std::vector<uint8_t> px, int w, int ch, const Rect& r, int rad) {
    const int k = 2 * rad + 1;
    std::vector<uint8_t> tmp = px;
    for (int y = r.y0; y < r.y1; ++y)
        for (int x = r.x0; x < r.x1; ++x)
            for (int c = 0; c < ch; ++c) {
                long acc = 0;
                for (int d = -rad; d <= rad; ++d)
                    acc += at(px, w, ch, std::clamp(x + d, r.x0, r.x1 - 1), y, c);
                tmp[(static_cast<size_t>(y) * w + x) * ch + c] = static_cast<uint8_t>(acc / k);
            }
    std::vector<uint8_t> out = tmp;
    for (int x = r.x0; x < r.x1; ++x)
        for (int y = r.y0; y < r.y1; ++y)
            for (int c = 0; c < ch; ++c) {
                long acc = 0;
                for (int d = -rad; d <= rad; ++d)
                    acc += at(tmp, w, ch, x, std::clamp(y + d, r.y0, r.y1 - 1), c);
                out[(static_cast<size_t>(y) * w + x) * ch + c] = static_cast<uint8_t>(acc / k);
            }
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// geometry
// ---------------------------------------------------------------------------
TEST(DynamicMaskGeometry, ExpandAddsPaddingAndClamps) {
    Rect r = expand_clamp(Box{100, 50, 40, 80}, 0.25f, 640, 480);
    EXPECT_EQ(r.x0, 90);  EXPECT_EQ(r.y0, 30);
    EXPECT_EQ(r.x1, 150); EXPECT_EQ(r.y1, 150);

    Rect edge = expand_clamp(Box{-10, 460, 50, 40}, 0.1f, 640, 480);
    EXPECT_EQ(edge.x0, 0);
    EXPECT_EQ(edge.y1, 480);
    EXPECT_TRUE(edge.valid());

    EXPECT_FALSE(expand_clamp(Box{700, 10, 20, 20}, 0.0f, 640, 480).valid());
    EXPECT_FALSE(expand_clamp(Box{10, 10, 0, 20}, 0.1f, 640, 480).valid());
}

TEST(DynamicMaskGeometry, HeadIsTopFraction) {
    Box h = head_of(Box{10, 20, 60, 180}, 0.3f);
    EXPECT_FLOAT_EQ(h.x, 10); EXPECT_FLOAT_EQ(h.y, 20);
    EXPECT_FLOAT_EQ(h.w, 60); EXPECT_FLOAT_EQ(h.h, 54);
}

// ---------------------------------------------------------------------------
// fills
// ---------------------------------------------------------------------------
TEST(DynamicMaskFill, BlackZerosOnlyTheRect) {
    const int w = 32, h = 24, ch = 3;
    auto before = noise(w, h, ch);
    auto px = before;
    const Rect r{5, 4, 20, 15};
    black_rect(px.data(), w, h, ch, r);
    for (int y = r.y0; y < r.y1; ++y)
        for (int x = r.x0; x < r.x1; ++x)
            for (int c = 0; c < ch; ++c) EXPECT_EQ(at(px, w, ch, x, y, c), 0);
    EXPECT_TRUE(outsideUnchanged(before, px, w, h, ch, r));
}

TEST(DynamicMaskFill, PixelateWritesBlockMeans) {
    const int w = 16, h = 16, ch = 1;
    std::vector<uint8_t> px(static_cast<size_t>(w) * h);
    std::iota(px.begin(), px.end(), 0);  // value = y*16 + x
    const auto before = px;
    const Rect r{0, 0, 8, 8};
    pixelate_rect(px.data(), w, h, ch, r, 4);
    // Top-left 4x4 block holds x,y in 0..3: mean = 1.5*16 + 1.5 = 25.5 -> 25.
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) EXPECT_EQ(at(px, w, ch, x, y, 0), 25);
    // Block x 4..7, y 4..7: mean 5.5*16 + 5.5 = 93.5 -> 93.
    EXPECT_EQ(at(px, w, ch, 6, 6, 0), 93);
    EXPECT_TRUE(outsideUnchanged(before, px, w, h, ch, r));
}

TEST(DynamicMaskFill, PixelateHandlesPartialEdgeBlocks) {
    const int w = 10, h = 10, ch = 3;
    auto px = noise(w, h, ch);
    const Rect r{2, 2, 9, 7};  // 7x5, block 4 leaves 3-wide and 1-tall remainders
    pixelate_rect(px.data(), w, h, ch, r, 4);
    // Every pixel in the trailing 3x1 block shares one value per channel.
    for (int c = 0; c < ch; ++c) {
        EXPECT_EQ(at(px, w, ch, 6, 6, c), at(px, w, ch, 8, 6, c));
    }
}

TEST(DynamicMaskFill, BlurMatchesNaiveReference) {
    const int w = 40, h = 30, ch = 3;
    const auto src = noise(w, h, ch, 11);
    const Rect r{6, 5, 31, 22};
    for (int rad : {1, 3, 7}) {
        auto fast = src;
        blur_rect(fast.data(), w, h, ch, r, rad, 1);
        const auto ref = referenceBlur(src, w, ch, r, rad);
        EXPECT_EQ(fast, ref) << "radius " << rad;
    }
}

TEST(DynamicMaskFill, BlurLeavesOutsideAloneAndFlattensNoise) {
    const int w = 64, h = 64, ch = 1;
    const auto src = noise(w, h, ch, 3);
    auto px = src;
    const Rect r{10, 10, 50, 50};
    blur_rect(px.data(), w, h, ch, r, 8, 2);
    EXPECT_TRUE(outsideUnchanged(src, px, w, h, ch, r));
    // Variance inside the rect drops by an order of magnitude.
    auto variance = [&](const std::vector<uint8_t>& v) {
        double sum = 0, sq = 0; int n = 0;
        for (int y = r.y0 + 8; y < r.y1 - 8; ++y)
            for (int x = r.x0 + 8; x < r.x1 - 8; ++x) {
                const double p = at(v, w, ch, x, y, 0); sum += p; sq += p * p; ++n;
            }
        const double m = sum / n; return sq / n - m * m;
    };
    EXPECT_LT(variance(px) * 10, variance(src));
}

TEST(DynamicMaskFill, InvalidOrOutOfFrameRectIsANoOp) {
    const int w = 8, h = 8, ch = 3;
    const auto src = noise(w, h, ch);
    auto px = src;
    black_rect(px.data(), w, h, ch, Rect{4, 4, 4, 6});
    pixelate_rect(px.data(), w, h, ch, Rect{0, 0, 9, 8}, 2);   // x1 past the frame
    blur_rect(px.data(), w, h, ch, Rect{0, 0, 8, 9}, 2);       // y1 past the frame
    EXPECT_EQ(px, src);
}

// ---------------------------------------------------------------------------
// box store
// ---------------------------------------------------------------------------
TEST(DynamicMaskStore, ExactPtsAndHoldWindow) {
    BoxStore s(400000);
    s.add(0, 1000000, {Box{1, 1, 10, 10}});
    EXPECT_EQ(s.lookup(0, 1000000).size(), 1u);
    EXPECT_EQ(s.lookup(0, 1300000).size(), 1u) << "held for a skipped frame";
    EXPECT_EQ(s.lookup(0, 800000).size(), 1u) << "a slightly later detection covers an earlier frame";
    EXPECT_TRUE(s.lookup(0, 1500000).empty()) << "past the hold window";
}

TEST(DynamicMaskStore, StreamsAreSeparate) {
    BoxStore s(400000);
    s.add(0, 1000000, {Box{1, 1, 10, 10}});
    EXPECT_TRUE(s.lookup(1, 1000000).empty());
}

TEST(DynamicMaskStore, EventsForTheSameFrameAccumulate) {
    BoxStore s(400000);
    s.add(0, 2000000, {Box{0, 0, 50, 150}});          // person from detect_onnx
    s.add(0, 2000000, {Box{10, 5, 20, 20}});          // face from recognize_face
    EXPECT_EQ(s.sets(0), 1u);
    EXPECT_EQ(s.lookup(0, 2000000).size(), 2u);
}

TEST(DynamicMaskStore, OldSetsArePrunedAndCapped) {
    BoxStore s(100000, 8);
    for (int i = 0; i < 20; ++i) s.add(0, 40000ull * i, {Box{0, 0, 1, 1}});
    EXPECT_EQ(s.sets(0), 8u) << "capped";
    s.lookup(0, 40000ull * 19);
    EXPECT_LE(s.sets(0), 3u) << "sets older than the hold window dropped on lookup";
}

TEST(DynamicMaskStore, EmptyEventsAreNotStored) {
    BoxStore s;
    s.add(0, 1, {});
    EXPECT_EQ(s.sets(0), 0u);
}
