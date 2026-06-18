// Unit tests for the pure LPR decode helpers (no ONNX Runtime required).

#include "../lpr_decode.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace zm::lpr;

TEST(NormalizePlate, UppercasesAndStripsNonAlnum) {
    EXPECT_EQ(normalize_plate("abc 123"), "ABC123");
    EXPECT_EQ(normalize_plate("a-b-c-1-2-3"), "ABC123");
    EXPECT_EQ(normalize_plate("  Ab1 "), "AB1");
    EXPECT_EQ(normalize_plate("!@#$%"), "");
    EXPECT_EQ(normalize_plate(""), "");
    EXPECT_EQ(normalize_plate("xyz789"), "XYZ789");
}

// Build a [T, C] logit matrix where, at each timestep, the class given by
// `seq[t]` has the dominant logit. Used to drive ctc_greedy_decode.
static std::vector<float> makeLogits(const std::vector<int>& seq, int C) {
    std::vector<float> logits(seq.size() * C, 0.0f);
    for (size_t t = 0; t < seq.size(); ++t) {
        logits[t * C + seq[t]] = 10.0f;  // dominant
    }
    return logits;
}

TEST(CtcGreedyDecode, CollapsesRepeatsAndDropsBlank) {
    // charset: index 0->'A', 1->'B', 2->'C', blank index = 3 (C-1).
    const std::string charset = "ABC";
    const int C = 4;  // 3 chars + blank
    const int blank = C - 1;

    // Timesteps:  A A blank A B B blank C
    // Expected collapse-repeats + drop-blank => "AABC"
    // (the blank between the two A's separates them so both survive).
    std::vector<int> seq = {0, 0, 3, 0, 1, 1, 3, 2};
    auto logits = makeLogits(seq, C);
    EXPECT_EQ(ctc_greedy_decode(logits.data(), (int)seq.size(), C, charset, blank), "AABC");
}

TEST(CtcGreedyDecode, SimpleNoBlankRepeats) {
    // "1 1 2 3 3 3" => collapse => "123"
    const std::string charset = "0123456789";
    const int C = 11;  // 10 digits + blank
    const int blank = C - 1;
    std::vector<int> seq = {1, 1, 2, 3, 3, 3};
    auto logits = makeLogits(seq, C);
    EXPECT_EQ(ctc_greedy_decode(logits.data(), (int)seq.size(), C, charset, blank), "123");
}

TEST(CtcGreedyDecode, DecodesKnownPlate) {
    // charset 36 alnum: 0-9 then A-Z. blank = 36 (C-1), C=37.
    const std::string charset = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    const int C = (int)charset.size() + 1;  // 37
    const int blank = C - 1;
    // Want "ABC123": A=10, B=11, C=12, 1=1, 2=2, 3=3.
    // Insert blanks and repeats to exercise collapse logic.
    // A A blank B C C blank 1 2 3
    std::vector<int> seq = {10, 10, blank, 11, 12, 12, blank, 1, 2, 3};
    auto logits = makeLogits(seq, C);
    EXPECT_EQ(ctc_greedy_decode(logits.data(), (int)seq.size(), C, charset, blank), "ABC123");
}

TEST(CtcGreedyDecode, ConfigurableBlankIndexZero) {
    // Blank at index 0; charset shifted so charset[0] is unused-by-blank.
    // Here charset[0]='_' won't be emitted because index 0 == blank.
    const std::string charset = "_AB";  // index0 blank slot, 1->A, 2->B
    const int C = 3;
    const int blank = 0;
    // 0 1 1 0 2 => blank A(rep) blank B => "AB"
    std::vector<int> seq = {0, 1, 1, 0, 2};
    auto logits = makeLogits(seq, C);
    EXPECT_EQ(ctc_greedy_decode(logits.data(), (int)seq.size(), C, charset, blank), "AB");
}

TEST(CtcGreedyDecode, EmptyInputs) {
    const std::string charset = "ABC";
    EXPECT_EQ(ctc_greedy_decode(nullptr, 5, 4, charset, 3), "");
    float dummy[1] = {0.0f};
    EXPECT_EQ(ctc_greedy_decode(dummy, 0, 4, charset, 3), "");
    EXPECT_EQ(ctc_greedy_decode(dummy, 5, 0, charset, 3), "");
}

TEST(Watchlisted, MatchesNormalized) {
    std::vector<std::string> list = {"ABC-123", "xyz 789"};
    EXPECT_TRUE(watchlisted(list, "abc123"));
    EXPECT_TRUE(watchlisted(list, "ABC 1 2 3"));
    EXPECT_TRUE(watchlisted(list, "XYZ789"));
    EXPECT_FALSE(watchlisted(list, "DEF456"));
    EXPECT_FALSE(watchlisted(list, ""));
    EXPECT_FALSE(watchlisted({}, "ABC123"));
}

TEST(CtcMeanConfidence, InRange) {
    const int C = 4;
    std::vector<int> seq = {0, 1, 2, 3};
    auto logits = makeLogits(seq, C);
    float conf = ctc_mean_confidence(logits.data(), (int)seq.size(), C);
    // Dominant logit of 10 vs zeros => softmax max prob near 1.0.
    EXPECT_GT(conf, 0.9f);
    EXPECT_LE(conf, 1.0f);
    EXPECT_FLOAT_EQ(ctc_mean_confidence(nullptr, 4, C), 0.0f);
}

TEST(CropResizeRgb, ProducesNormalizedValues) {
    // 2x2 white image, crop full, resize to 2x2 => all 1.0.
    std::vector<uint8_t> img(2 * 2 * 3, 255);
    std::vector<float> dst(3 * 2 * 2, -1.0f);
    crop_resize_rgb(img.data(), 2, 2, 0, 0, 2, 2, 2, 2, dst.data());
    for (float v : dst) EXPECT_NEAR(v, 1.0f, 1e-5f);
}

TEST(CropResizeGray, ProducesNormalizedValues) {
    std::vector<uint8_t> img(2 * 2 * 3, 255);
    std::vector<float> dst(2 * 2, -1.0f);
    crop_resize_gray(img.data(), 2, 2, 0, 0, 2, 2, 2, 2, dst.data());
    for (float v : dst) EXPECT_NEAR(v, 1.0f, 1e-5f);
}
