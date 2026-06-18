// Unit tests for the pure post-processing helpers in seg_postprocess.hpp.
// These do not require ONNX Runtime.

#include "../seg_postprocess.hpp"

#include <gtest/gtest.h>
#include <vector>
#include <array>
#include <cmath>

using namespace zm::seg;

static SegObj makeBox(float x, float y, float w, float h, float conf, int cls) {
    SegObj o;
    o.x = x; o.y = y; o.w = w; o.h = h;
    o.confidence = conf; o.class_id = cls;
    return o;
}

TEST(Iou, IdenticalBoxesIsOne) {
    SegObj a = makeBox(0, 0, 10, 10, 0.9f, 0);
    SegObj b = makeBox(0, 0, 10, 10, 0.8f, 0);
    EXPECT_FLOAT_EQ(iou(a, b), 1.0f);
}

TEST(Iou, DisjointBoxesIsZero) {
    SegObj a = makeBox(0, 0, 10, 10, 0.9f, 0);
    SegObj b = makeBox(100, 100, 10, 10, 0.8f, 0);
    EXPECT_FLOAT_EQ(iou(a, b), 0.0f);
}

TEST(Iou, HalfOverlap) {
    // Two 10x10 boxes overlapping in a 5x10 region.
    SegObj a = makeBox(0, 0, 10, 10, 0.9f, 0);
    SegObj b = makeBox(5, 0, 10, 10, 0.8f, 0);
    // inter = 5*10 = 50; union = 100+100-50 = 150 -> 1/3.
    EXPECT_NEAR(iou(a, b), 50.0f / 150.0f, 1e-6f);
}

TEST(Nms, SuppressesHighOverlapSameClass) {
    std::vector<SegObj> objs = {
        makeBox(0, 0, 10, 10, 0.9f, 0),
        makeBox(1, 1, 10, 10, 0.8f, 0),   // ~64% IoU -> suppressed
        makeBox(100, 100, 10, 10, 0.7f, 0),
    };
    auto kept = nms(objs, 0.45f);
    ASSERT_EQ(kept.size(), 2u);
    EXPECT_FLOAT_EQ(kept[0].confidence, 0.9f);
    EXPECT_FLOAT_EQ(kept[1].confidence, 0.7f);
}

TEST(Nms, DoesNotSuppressAcrossClasses) {
    std::vector<SegObj> objs = {
        makeBox(0, 0, 10, 10, 0.9f, 0),
        makeBox(0, 0, 10, 10, 0.8f, 1),  // identical box, different class
    };
    auto kept = nms(objs, 0.45f);
    EXPECT_EQ(kept.size(), 2u);
}

TEST(Sigmoid, KnownValues) {
    EXPECT_NEAR(sigmoid(0.0f), 0.5f, 1e-6f);
    EXPECT_NEAR(sigmoid(100.0f), 1.0f, 1e-6f);
    EXPECT_NEAR(sigmoid(-100.0f), 0.0f, 1e-6f);
}

TEST(BuildMask, CoeffTimesProtoSigmoid) {
    // proto: nm=2, mh=2, mw=2 -> 2 planes of 4 values.
    // plane0 = all 1, plane1 = all -1.
    // coeffs = {3, 1} -> linear = 3*1 + 1*(-1) = 2 everywhere -> sigmoid(2).
    const int nm = 2, mh = 2, mw = 2;
    std::vector<float> proto = {
        1, 1, 1, 1,      // plane 0
        -1, -1, -1, -1,  // plane 1
    };
    std::vector<float> coeffs = {3.0f, 1.0f};
    auto mask = build_mask(proto.data(), nm, mh, mw, coeffs);
    ASSERT_EQ(mask.size(), 4u);
    const float expected = 1.0f / (1.0f + std::exp(-2.0f));
    for (float v : mask) EXPECT_NEAR(v, expected, 1e-6f);
}

TEST(BuildMask, NonUniformPlanes) {
    // plane0 distinct values, plane1 zeros, coeffs {1,0} -> sigmoid(plane0).
    const int nm = 2, mh = 1, mw = 3;
    std::vector<float> proto = {
        -2, 0, 5,   // plane 0
        9, 9, 9,    // plane 1 (zeroed by coeff 0)
    };
    std::vector<float> coeffs = {1.0f, 0.0f};
    auto mask = build_mask(proto.data(), nm, mh, mw, coeffs);
    ASSERT_EQ(mask.size(), 3u);
    EXPECT_NEAR(mask[0], 1.0f / (1.0f + std::exp(2.0f)), 1e-6f);
    EXPECT_NEAR(mask[1], 0.5f, 1e-6f);
    EXPECT_NEAR(mask[2], 1.0f / (1.0f + std::exp(-5.0f)), 1e-6f);
}

TEST(MaskToPolygon, KnownSquareMask) {
    // Identity letterbox: src 8x8 into net 8, mask grid 8x8 (scale 1, no pad).
    // A 2x2 block of "on" pixels at mask rows 2-3, cols 2-3.
    zm::detect::Letterbox lb = zm::detect::compute_letterbox(8, 8, 8);
    const int mh = 8, mw = 8;
    std::vector<float> mask(static_cast<size_t>(mh) * mw, 0.0f);
    for (int y = 2; y <= 3; ++y)
        for (int x = 2; x <= 3; ++x)
            mask[y * mw + x] = 1.0f;

    SegObj box = makeBox(0, 0, 8, 8, 0.9f, 0);  // box covers whole frame
    auto poly = mask_to_polygon(mask, mh, mw, 0.5f, lb, box, /*row_step=*/1);

    // Two occupied rows (y=2,3) -> 2 left + 2 right = 4 points.
    ASSERT_EQ(poly.size(), 4u);
    // mask->net is identity here (net/mw = 1); points stay in [0,8].
    for (const auto& p : poly) {
        EXPECT_GE(p[0], 0.0f);
        EXPECT_LE(p[0], 8.0f);
        EXPECT_GE(p[1], 0.0f);
        EXPECT_LE(p[1], 8.0f);
    }
    // First left point at mask col 2 -> source x = 2.
    EXPECT_NEAR(poly[0][0], 2.0f, 1e-4f);
    EXPECT_NEAR(poly[0][1], 2.0f, 1e-4f);
}

TEST(MaskToPolygon, EmptyMaskGivesEmptyPolygon) {
    zm::detect::Letterbox lb = zm::detect::compute_letterbox(8, 8, 8);
    std::vector<float> mask(64, 0.0f);
    SegObj box = makeBox(0, 0, 8, 8, 0.9f, 0);
    auto poly = mask_to_polygon(mask, 8, 8, 0.5f, lb, box, 1);
    EXPECT_TRUE(poly.empty());
}

TEST(Decode, ChannelMajorArgmaxAndBox) {
    // channel-major: [1, channels, num], channels = 4 + nc(2) + nm(2) = 8, num=1.
    // Identity letterbox 8x8 net 8. Candidate centered box (cx=4,cy=4,w=4,h=4).
    zm::detect::Letterbox lb = zm::detect::compute_letterbox(8, 8, 8);
    const int num = 1, nc = 2, nm = 2;
    const int channels = 4 + nc + nm;
    std::vector<float> det(static_cast<size_t>(channels) * num, 0.0f);
    auto set = [&](int c, float v) { det[c * num + 0] = v; };
    set(0, 4.0f); set(1, 4.0f); set(2, 4.0f); set(3, 4.0f);  // box
    set(4, 0.2f); set(5, 0.8f);   // class scores -> argmax class 1, conf 0.8
    set(6, 1.5f); set(7, -0.5f);  // mask coeffs

    auto objs = decode(det.data(), num, channels, nc, nm, /*channel_major=*/true,
                       lb, /*conf_thr=*/0.25f);
    ASSERT_EQ(objs.size(), 1u);
    EXPECT_EQ(objs[0].class_id, 1);
    EXPECT_NEAR(objs[0].confidence, 0.8f, 1e-6f);
    // xyxy = (2,2)-(6,6) -> source (identity) x=2,y=2,w=4,h=4.
    EXPECT_NEAR(objs[0].x, 2.0f, 1e-4f);
    EXPECT_NEAR(objs[0].y, 2.0f, 1e-4f);
    EXPECT_NEAR(objs[0].w, 4.0f, 1e-4f);
    EXPECT_NEAR(objs[0].h, 4.0f, 1e-4f);
    ASSERT_EQ(objs[0].coeffs.size(), 2u);
    EXPECT_NEAR(objs[0].coeffs[0], 1.5f, 1e-6f);
    EXPECT_NEAR(objs[0].coeffs[1], -0.5f, 1e-6f);
}

TEST(Decode, ConfidenceThresholdRejects) {
    zm::detect::Letterbox lb = zm::detect::compute_letterbox(8, 8, 8);
    const int num = 1, nc = 2, nm = 2;
    const int channels = 4 + nc + nm;
    std::vector<float> det(static_cast<size_t>(channels) * num, 0.0f);
    auto set = [&](int c, float v) { det[c * num + 0] = v; };
    set(0, 4.0f); set(1, 4.0f); set(2, 4.0f); set(3, 4.0f);
    set(4, 0.1f); set(5, 0.1f);  // both below 0.25
    auto objs = decode(det.data(), num, channels, nc, nm, true, lb, 0.25f);
    EXPECT_TRUE(objs.empty());
}
