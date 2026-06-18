// Unit tests for the pure YOLO-pose post-processing (iou, nms, decode).
// No ONNX Runtime needed.

#include "../pose_postprocess.hpp"

#include <gtest/gtest.h>

#include <vector>
#include <cmath>

using zm::pose::Person;
using zm::pose::Keypoint;
using zm::detect::Letterbox;

namespace {

Person makePerson(float x, float y, float w, float h, float conf) {
    Person p;
    p.x = x; p.y = y; p.w = w; p.h = h; p.confidence = conf;
    return p;
}

} // namespace

TEST(PoseIoU, IdenticalBoxesIsOne) {
    Person a = makePerson(10, 10, 100, 100, 0.9f);
    Person b = makePerson(10, 10, 100, 100, 0.8f);
    EXPECT_NEAR(zm::pose::iou(a, b), 1.0f, 1e-5f);
}

TEST(PoseIoU, DisjointBoxesIsZero) {
    Person a = makePerson(0, 0, 10, 10, 0.9f);
    Person b = makePerson(100, 100, 10, 10, 0.8f);
    EXPECT_NEAR(zm::pose::iou(a, b), 0.0f, 1e-5f);
}

TEST(PoseIoU, HalfOverlap) {
    // a = [0,0,100,100], b = [50,0,100,100] -> inter 50*100=5000,
    // union = 10000 + 10000 - 5000 = 15000 -> 1/3.
    Person a = makePerson(0, 0, 100, 100, 0.9f);
    Person b = makePerson(50, 0, 100, 100, 0.8f);
    EXPECT_NEAR(zm::pose::iou(a, b), 1.0f / 3.0f, 1e-4f);
}

TEST(PoseNMS, SuppressesOverlappingLowerConfidence) {
    std::vector<Person> in;
    in.push_back(makePerson(10, 10, 100, 100, 0.9f));  // keep
    in.push_back(makePerson(12, 12, 100, 100, 0.7f));  // suppressed (IoU high)
    in.push_back(makePerson(300, 300, 50, 50, 0.6f));  // keep (disjoint)
    auto kept = zm::pose::nms(in, 0.45f);
    ASSERT_EQ(kept.size(), 2u);
    // Sorted by confidence descending.
    EXPECT_FLOAT_EQ(kept[0].confidence, 0.9f);
    EXPECT_FLOAT_EQ(kept[1].confidence, 0.6f);
}

TEST(PoseNMS, KeepsAllWhenNoOverlap) {
    std::vector<Person> in;
    in.push_back(makePerson(0, 0, 10, 10, 0.5f));
    in.push_back(makePerson(100, 100, 10, 10, 0.9f));
    in.push_back(makePerson(200, 200, 10, 10, 0.7f));
    auto kept = zm::pose::nms(in, 0.45f);
    EXPECT_EQ(kept.size(), 3u);
}

TEST(PoseNMS, CarriesKeypointsThrough) {
    Person p = makePerson(10, 10, 100, 100, 0.9f);
    p.kpts.push_back(Keypoint{42.0f, 24.0f, 0.95f});
    std::vector<Person> in{p};
    auto kept = zm::pose::nms(in, 0.45f);
    ASSERT_EQ(kept.size(), 1u);
    ASSERT_EQ(kept[0].kpts.size(), 1u);
    EXPECT_FLOAT_EQ(kept[0].kpts[0].x, 42.0f);
    EXPECT_FLOAT_EQ(kept[0].kpts[0].y, 24.0f);
    EXPECT_FLOAT_EQ(kept[0].kpts[0].v, 0.95f);
}

TEST(PoseDecode, CandidateMajorSimpleCase) {
    // Square source 640x640 -> letterbox is identity (scale=1, pad=0).
    Letterbox lb = zm::detect::compute_letterbox(640, 640, 640);
    ASSERT_FLOAT_EQ(lb.scale, 1.0f);
    ASSERT_EQ(lb.pad_x, 0);
    ASSERT_EQ(lb.pad_y, 0);

    const int numKpts = 1;
    const int values = 5 + numKpts * 3;  // 8
    const int num = 2;
    std::vector<float> out(static_cast<size_t>(num) * values, 0.0f);

    // Candidate 0: center (100,100) size (40,60), conf 0.9, kpt (105,95,0.8).
    float* c0 = out.data() + 0 * values;
    c0[0] = 100; c0[1] = 100; c0[2] = 40; c0[3] = 60; c0[4] = 0.9f;
    c0[5] = 105; c0[6] = 95;  c0[7] = 0.8f;

    // Candidate 1: low confidence, should be filtered out.
    float* c1 = out.data() + 1 * values;
    c1[0] = 300; c1[1] = 300; c1[2] = 20; c1[3] = 20; c1[4] = 0.1f;
    c1[5] = 300; c1[6] = 300; c1[7] = 0.2f;

    auto persons = zm::pose::decode(out.data(), num, numKpts,
                                    /*channel_major=*/false, lb, 0.25f);
    ASSERT_EQ(persons.size(), 1u);
    const auto& p = persons[0];
    EXPECT_FLOAT_EQ(p.confidence, 0.9f);
    EXPECT_NEAR(p.x, 80.0f, 1e-3f);   // 100 - 40/2
    EXPECT_NEAR(p.y, 70.0f, 1e-3f);   // 100 - 60/2
    EXPECT_NEAR(p.w, 40.0f, 1e-3f);
    EXPECT_NEAR(p.h, 60.0f, 1e-3f);
    ASSERT_EQ(p.kpts.size(), 1u);
    EXPECT_NEAR(p.kpts[0].x, 105.0f, 1e-3f);
    EXPECT_NEAR(p.kpts[0].y, 95.0f, 1e-3f);
    EXPECT_FLOAT_EQ(p.kpts[0].v, 0.8f);
}

TEST(PoseDecode, ChannelMajorMatchesCandidateMajor) {
    Letterbox lb = zm::detect::compute_letterbox(640, 640, 640);
    const int numKpts = 1;
    const int values = 5 + numKpts * 3;  // 8
    const int num = 2;

    // Build a candidate-major buffer.
    std::vector<float> cm(static_cast<size_t>(num) * values, 0.0f);
    float c[2][8] = {
        {100, 100, 40, 60, 0.9f, 105, 95, 0.8f},
        {200, 220, 30, 30, 0.7f, 205, 215, 0.6f},
    };
    for (int i = 0; i < num; ++i)
        for (int v = 0; v < values; ++v)
            cm[i * values + v] = c[i][v];

    // Build the channel-major transpose: out[v*num + i].
    std::vector<float> chm(static_cast<size_t>(num) * values, 0.0f);
    for (int i = 0; i < num; ++i)
        for (int v = 0; v < values; ++v)
            chm[v * num + i] = c[i][v];

    auto a = zm::pose::decode(cm.data(), num, numKpts, false, lb, 0.25f);
    auto b = zm::pose::decode(chm.data(), num, numKpts, true, lb, 0.25f);
    ASSERT_EQ(a.size(), b.size());
    ASSERT_EQ(a.size(), 2u);
    for (size_t i = 0; i < a.size(); ++i) {
        EXPECT_FLOAT_EQ(a[i].confidence, b[i].confidence);
        EXPECT_FLOAT_EQ(a[i].x, b[i].x);
        EXPECT_FLOAT_EQ(a[i].y, b[i].y);
        EXPECT_FLOAT_EQ(a[i].w, b[i].w);
        EXPECT_FLOAT_EQ(a[i].h, b[i].h);
        ASSERT_EQ(a[i].kpts.size(), b[i].kpts.size());
        EXPECT_FLOAT_EQ(a[i].kpts[0].x, b[i].kpts[0].x);
        EXPECT_FLOAT_EQ(a[i].kpts[0].y, b[i].kpts[0].y);
    }
}

TEST(PoseDecode, LetterboxUnmapping) {
    // Non-square source 1280x720 -> scale = 640/1280 = 0.5, pad_y added.
    Letterbox lb = zm::detect::compute_letterbox(1280, 720, 640);
    ASSERT_FLOAT_EQ(lb.scale, 0.5f);
    EXPECT_EQ(lb.pad_x, 0);
    EXPECT_EQ(lb.pad_y, (640 - 360) / 2);  // 140

    const int numKpts = 1;
    const int values = 8;
    std::vector<float> out(values, 0.0f);
    // A keypoint at net (320, 320): source x = 320/0.5 = 640,
    // source y = (320 - 140)/0.5 = 360.
    out[0] = 320; out[1] = 320; out[2] = 100; out[3] = 100; out[4] = 0.9f;
    out[5] = 320; out[6] = 320; out[7] = 0.99f;

    auto persons = zm::pose::decode(out.data(), 1, numKpts, false, lb, 0.25f);
    ASSERT_EQ(persons.size(), 1u);
    EXPECT_NEAR(persons[0].kpts[0].x, 640.0f, 1e-2f);
    EXPECT_NEAR(persons[0].kpts[0].y, 360.0f, 1e-2f);
}
