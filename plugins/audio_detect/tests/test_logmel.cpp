// Unit tests for the pure log-mel front-end (logmel.hpp). No ONNX/FFmpeg needed.

#include "logmel.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

using namespace zm::audio;

namespace {
std::vector<float> tone(int n, float freq, int sr) {
    std::vector<float> x(n);
    for (int i = 0; i < n; ++i)
        x[i] = std::cos(2.f * static_cast<float>(M_PI) * freq * i / sr);
    return x;
}
}  // namespace

TEST(LogMel, FftPeaksAtSignalBin) {
    const size_t N = 64;
    const int k = 8;
    std::vector<float> re(N), im(N, 0.f);
    for (size_t i = 0; i < N; ++i)
        re[i] = std::cos(2.f * static_cast<float>(M_PI) * k * i / N);
    fft_pow2(re, im);
    // Power spectrum; a real cosine peaks at bins k and N-k.
    size_t argmax = 0; float best = -1.f;
    for (size_t b = 1; b < N / 2 + 1; ++b) {
        const float p = re[b] * re[b] + im[b] * im[b];
        if (p > best) { best = p; argmax = b; }
    }
    EXPECT_EQ(argmax, static_cast<size_t>(k));
}

TEST(LogMel, Pow2Check) {
    EXPECT_TRUE(is_pow2(512));
    EXPECT_FALSE(is_pow2(400));
    EXPECT_FALSE(is_pow2(0));
}

TEST(LogMel, FilterbankShapeAndNonNegative) {
    MelConfig c; c.sample_rate = 16000; c.n_fft = 64; c.n_mels = 8;
    MelExtractor mx(c);
    ASSERT_TRUE(mx.valid());
    const auto& fb = mx.filterbank();
    ASSERT_EQ(fb.size(), 8u);
    const int nbins = c.n_fft / 2 + 1;
    for (const auto& row : fb) {
        ASSERT_EQ(static_cast<int>(row.size()), nbins);
        for (float w : row) EXPECT_GE(w, 0.f);
    }
}

TEST(LogMel, ExtractShapeAndFinite) {
    MelConfig c; c.sample_rate = 16000; c.n_fft = 512; c.hop_length = 160; c.n_mels = 64;
    MelExtractor mx(c);
    const int n = 2048;
    auto x = tone(n, 1000.f, c.sample_rate);
    int frames = 0;
    auto mel = mx.extract(x.data(), x.size(), frames);
    EXPECT_EQ(frames, 1 + (n - c.n_fft) / c.hop_length);   // = 10
    ASSERT_EQ(mel.size(), static_cast<size_t>(c.n_mels) * frames);
    for (float v : mel) EXPECT_TRUE(std::isfinite(v));
}

TEST(LogMel, EnergyConcentratesNearToneFrequency) {
    MelConfig c; c.sample_rate = 16000; c.n_fft = 512; c.hop_length = 256; c.n_mels = 64;
    MelExtractor mx(c);
    const int n = 4096;
    auto x = tone(n, 1000.f, c.sample_rate);   // a 1 kHz tone
    int frames = 0;
    auto mel = mx.extract(x.data(), x.size(), frames);
    ASSERT_GT(frames, 0);
    // Mel band centres rise with index; ~1 kHz lands in a low-mid band, ~7 kHz high.
    // The low-mid band should carry more energy than a high band for a 1 kHz tone.
    auto bandVal = [&](int m) { return mel[static_cast<size_t>(m) * frames + 0]; };
    float lowmid = bandVal(15);   // ~1 kHz region
    float high = bandVal(60);     // near Nyquist
    EXPECT_GT(lowmid, high);
}

TEST(LogMel, ShortWindowYieldsNoFrames) {
    MelConfig c; c.n_fft = 512;
    MelExtractor mx(c);
    std::vector<float> x(100, 0.1f);  // shorter than n_fft
    int frames = -1;
    auto mel = mx.extract(x.data(), x.size(), frames);
    EXPECT_EQ(frames, 0);
    EXPECT_TRUE(mel.empty());
}
