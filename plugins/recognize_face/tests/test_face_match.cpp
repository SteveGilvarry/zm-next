// Unit tests for the pure face-matching helpers (no ONNX Runtime).

#include "../face_match.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

using namespace zm::face;

TEST(L2Normalize, ProducesUnitVector) {
    std::vector<float> v{3.0f, 4.0f};
    l2_normalize(v);
    EXPECT_NEAR(v[0], 0.6f, 1e-5);
    EXPECT_NEAR(v[1], 0.8f, 1e-5);
    const float norm = std::sqrt(v[0] * v[0] + v[1] * v[1]);
    EXPECT_NEAR(norm, 1.0f, 1e-5);
}

TEST(L2Normalize, ZeroVectorUnchanged) {
    std::vector<float> v{0.0f, 0.0f, 0.0f};
    l2_normalize(v);
    EXPECT_FLOAT_EQ(v[0], 0.0f);
    EXPECT_FLOAT_EQ(v[1], 0.0f);
    EXPECT_FLOAT_EQ(v[2], 0.0f);
}

TEST(Cosine, IdenticalNormalizedIsOne) {
    std::vector<float> a{1.0f, 0.0f, 0.0f};
    EXPECT_NEAR(cosine(a, a), 1.0f, 1e-6);
}

TEST(Cosine, OrthogonalIsZero) {
    std::vector<float> a{1.0f, 0.0f};
    std::vector<float> b{0.0f, 1.0f};
    EXPECT_NEAR(cosine(a, b), 0.0f, 1e-6);
}

TEST(Cosine, OppositeIsNegativeOne) {
    std::vector<float> a{1.0f, 0.0f};
    std::vector<float> b{-1.0f, 0.0f};
    EXPECT_NEAR(cosine(a, b), -1.0f, 1e-6);
}

TEST(Cosine, SizeMismatchIsZero) {
    std::vector<float> a{1.0f, 0.0f};
    std::vector<float> b{1.0f};
    EXPECT_FLOAT_EQ(cosine(a, b), 0.0f);
    EXPECT_FLOAT_EQ(cosine({}, {}), 0.0f);
}

TEST(BestMatch, ReturnsNamedAboveThreshold) {
    std::vector<GalleryEntry> g{
        {"alice", {1.0f, 0.0f, 0.0f}},
        {"bob",   {0.0f, 1.0f, 0.0f}},
    };
    std::vector<float> probe{0.99f, 0.14f, 0.0f};
    l2_normalize(probe);
    Match m = best_match(g, probe, 0.5f);
    EXPECT_EQ(m.name, "alice");
    EXPECT_GT(m.score, 0.5f);
}

TEST(BestMatch, ReturnsUnknownBelowThreshold) {
    std::vector<GalleryEntry> g{
        {"alice", {1.0f, 0.0f, 0.0f}},
    };
    std::vector<float> probe{0.0f, 1.0f, 0.0f};  // orthogonal -> score 0
    Match m = best_match(g, probe, 0.5f);
    EXPECT_EQ(m.name, "unknown");
    EXPECT_NEAR(m.score, 0.0f, 1e-6);
}

TEST(BestMatch, EmptyGalleryIsUnknown) {
    std::vector<GalleryEntry> g;
    std::vector<float> probe{1.0f, 0.0f};
    Match m = best_match(g, probe, 0.5f);
    EXPECT_EQ(m.name, "unknown");
    EXPECT_FLOAT_EQ(m.score, 0.0f);
}

TEST(BestMatch, PicksHighestScore) {
    std::vector<GalleryEntry> g{
        {"alice", {1.0f, 0.0f}},
        {"bob",   {0.7071f, 0.7071f}},
    };
    std::vector<float> probe{0.9f, 0.1f};
    l2_normalize(probe);
    Match m = best_match(g, probe, 0.0f);
    EXPECT_EQ(m.name, "alice");
}

TEST(CropResize, SolidColorPreserved) {
    // 4x4 solid red source -> 2x2 crop should stay red.
    const int sw = 4, sh = 4;
    std::vector<uint8_t> src(static_cast<size_t>(sw) * sh * 3, 0);
    for (size_t i = 0; i < src.size(); i += 3) {
        src[i] = 200;      // R
        src[i + 1] = 50;   // G
        src[i + 2] = 10;   // B
    }
    std::vector<uint8_t> out;
    crop_resize_rgb(src.data(), sw, sh, 1, 1, 2, 2, 2, 2, out);
    ASSERT_EQ(out.size(), static_cast<size_t>(2 * 2 * 3));
    for (size_t i = 0; i < out.size(); i += 3) {
        EXPECT_EQ(out[i], 200);
        EXPECT_EQ(out[i + 1], 50);
        EXPECT_EQ(out[i + 2], 10);
    }
}

TEST(CropResize, OutputSizeAndDegenerateRect) {
    const int sw = 8, sh = 8;
    std::vector<uint8_t> src(static_cast<size_t>(sw) * sh * 3, 128);
    std::vector<uint8_t> out;

    // Upscale a 2x2 region to 4x4.
    crop_resize_rgb(src.data(), sw, sh, 0, 0, 2, 2, 4, 4, out);
    EXPECT_EQ(out.size(), static_cast<size_t>(4 * 4 * 3));

    // Out-of-bounds rect -> coordinates are clamped to the source edge, so the
    // output samples valid (edge) pixels (here the whole source is 128).
    crop_resize_rgb(src.data(), sw, sh, 100, 100, 5, 5, 3, 3, out);
    ASSERT_EQ(out.size(), static_cast<size_t>(3 * 3 * 3));
    for (uint8_t v : out) EXPECT_EQ(v, 128);
}
