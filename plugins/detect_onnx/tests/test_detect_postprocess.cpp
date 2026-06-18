// Unit tests for the pure pre/post-processing helpers in detect_postprocess.hpp.
// These do not require ONNX Runtime.

#include "../detect_postprocess.hpp"

#include <gtest/gtest.h>
#include <vector>
#include <cmath>

using namespace zm::detect;

TEST(Letterbox, NonSquareScaleAndPad) {
    // 1280x720 into a 640 network: scale limited by width (640/1280 = 0.5),
    // new size 640x360, so vertical padding of (640-360)/2 = 140.
    Letterbox lb = compute_letterbox(1280, 720, 640);
    EXPECT_EQ(lb.net, 640);
    EXPECT_EQ(lb.src_w, 1280);
    EXPECT_EQ(lb.src_h, 720);
    EXPECT_FLOAT_EQ(lb.scale, 0.5f);
    EXPECT_EQ(lb.pad_x, 0);
    EXPECT_EQ(lb.pad_y, 140);
}

TEST(Letterbox, SquareImageNoPad) {
    Letterbox lb = compute_letterbox(640, 640, 640);
    EXPECT_FLOAT_EQ(lb.scale, 1.0f);
    EXPECT_EQ(lb.pad_x, 0);
    EXPECT_EQ(lb.pad_y, 0);
}

TEST(Unletterbox, RoundTripsKnownBox) {
    // Same letterbox as above (scale 0.5, pad_y 140). A net-space box should
    // map back to verifiable source pixels.
    Letterbox lb = compute_letterbox(1280, 720, 640);
    // net-space xyxy (100,200)-(300,400)
    Box b = unletterbox_xyxy(100.f, 200.f, 300.f, 400.f, lb);
    // sx1 = (100-0)/0.5 = 200 ; sy1 = (200-140)/0.5 = 120
    // sx2 = (300-0)/0.5 = 600 ; sy2 = (400-140)/0.5 = 520
    EXPECT_FLOAT_EQ(b.x, 200.f);
    EXPECT_FLOAT_EQ(b.y, 120.f);
    EXPECT_FLOAT_EQ(b.w, 400.f);
    EXPECT_FLOAT_EQ(b.h, 400.f);
}

TEST(DecodeNmsFree, FiltersByConfidence) {
    Letterbox lb = compute_letterbox(1280, 720, 640);
    // Two rows: [x1,y1,x2,y2,conf,class]
    std::vector<float> out = {
        100.f, 200.f, 300.f, 400.f, 0.90f, 0.f,  // kept
        100.f, 200.f, 300.f, 400.f, 0.10f, 1.f,  // dropped (low conf)
    };
    auto boxes = decode_nms_free(out.data(), 2, lb, 0.25f);
    ASSERT_EQ(boxes.size(), 1u);
    EXPECT_EQ(boxes[0].class_id, 0);
    EXPECT_FLOAT_EQ(boxes[0].confidence, 0.90f);
    EXPECT_FLOAT_EQ(boxes[0].x, 200.f);
    EXPECT_FLOAT_EQ(boxes[0].y, 120.f);
    EXPECT_FLOAT_EQ(boxes[0].w, 400.f);
    EXPECT_FLOAT_EQ(boxes[0].h, 400.f);
}

TEST(DecodeNmsFree, FiltersByClassAllowList) {
    Letterbox lb = compute_letterbox(1280, 720, 640);
    std::vector<float> out = {
        100.f, 200.f, 300.f, 400.f, 0.90f, 0.f,  // class 0 (person)
        110.f, 210.f, 310.f, 410.f, 0.95f, 2.f,  // class 2 (car)
        120.f, 220.f, 320.f, 420.f, 0.99f, 7.f,  // class 7 (truck)
    };
    // Allow only class 2.
    auto boxes = decode_nms_free(out.data(), 3, lb, 0.25f, {2});
    ASSERT_EQ(boxes.size(), 1u);
    EXPECT_EQ(boxes[0].class_id, 2);
    EXPECT_FLOAT_EQ(boxes[0].confidence, 0.95f);
}

TEST(DecodeNmsFree, EmptyAllowListKeepsAll) {
    Letterbox lb = compute_letterbox(640, 640, 640);
    std::vector<float> out = {
        10.f, 10.f, 50.f, 50.f, 0.5f, 0.f,
        20.f, 20.f, 60.f, 60.f, 0.5f, 5.f,
    };
    auto boxes = decode_nms_free(out.data(), 2, lb, 0.25f, {});
    EXPECT_EQ(boxes.size(), 2u);
}

TEST(DecodeNmsFree, DropsDegenerateBox) {
    Letterbox lb = compute_letterbox(640, 640, 640);
    // x2 < x1 -> non-positive width, should be dropped.
    std::vector<float> out = {
        300.f, 300.f, 100.f, 100.f, 0.9f, 0.f,
    };
    auto boxes = decode_nms_free(out.data(), 1, lb, 0.25f);
    EXPECT_TRUE(boxes.empty());
}
