#pragma once

// Pure, dependency-free log-mel spectrogram front-end for audio_detect.
//
// YAMNet-style models take a raw waveform; modern AudioSet taggers (CED /
// EfficientAT / PaSST) take a LOG-MEL spectrogram. This header turns a mono float
// PCM window into a [n_mels, n_frames] log-mel feature so audio_detect can drive
// either model family. All parameters are configurable so the front-end can be
// matched to whatever model is loaded (n_fft / hop / n_mels / fmin / fmax / log).
//
// Pipeline: Hann-windowed frames -> radix-2 FFT -> power spectrum -> triangular
// mel filterbank (HTK mel scale, optional Slaney area-normalization) -> log.
// No external FFT/DSP dependency (small iterative Cooley-Tukey FFT below).

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace zm::audio {

struct MelConfig {
    int sample_rate = 16000;
    int n_fft = 512;        // must be a power of two
    int hop_length = 160;
    int n_mels = 64;
    float fmin = 0.f;
    float fmax = 0.f;       // 0 => sample_rate / 2
    float log_offset = 1e-6f;
    bool log10 = false;     // false: natural log; true: 10*log10 (dB-ish)
    bool slaney = false;    // Slaney area-normalize the mel filters
};

inline bool is_pow2(int n) { return n > 0 && (n & (n - 1)) == 0; }

inline float hz_to_mel(float hz) { return 2595.f * std::log10(1.f + hz / 700.f); }
inline float mel_to_hz(float mel) { return 700.f * (std::pow(10.f, mel / 2595.f) - 1.f); }

// In-place iterative radix-2 Cooley-Tukey FFT. re/im are size N (power of two).
inline void fft_pow2(std::vector<float>& re, std::vector<float>& im) {
    const size_t N = re.size();
    if (N < 2) return;
    // Bit-reversal permutation.
    for (size_t i = 1, j = 0; i < N; ++i) {
        size_t bit = N >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
    }
    for (size_t len = 2; len <= N; len <<= 1) {
        const float ang = -2.f * static_cast<float>(M_PI) / static_cast<float>(len);
        const float wr = std::cos(ang), wi = std::sin(ang);
        for (size_t i = 0; i < N; i += len) {
            float cwr = 1.f, cwi = 0.f;
            for (size_t k = 0; k < len / 2; ++k) {
                const size_t a = i + k, b = i + k + len / 2;
                const float ur = re[a], ui = im[a];
                const float vr = re[b] * cwr - im[b] * cwi;
                const float vi = re[b] * cwi + im[b] * cwr;
                re[a] = ur + vr; im[a] = ui + vi;
                re[b] = ur - vr; im[b] = ui - vi;
                const float ncwr = cwr * wr - cwi * wi;
                cwi = cwr * wi + cwi * wr;
                cwr = ncwr;
            }
        }
    }
}

class MelExtractor {
public:
    explicit MelExtractor(const MelConfig& cfg) : cfg_(cfg) {
        if (cfg_.fmax <= 0.f) cfg_.fmax = cfg_.sample_rate * 0.5f;
        nbins_ = cfg_.n_fft / 2 + 1;
        // Hann window.
        window_.resize(cfg_.n_fft);
        for (int i = 0; i < cfg_.n_fft; ++i)
            window_[i] = 0.5f - 0.5f * std::cos(2.f * static_cast<float>(M_PI) * i /
                                                (cfg_.n_fft - 1));
        build_filterbank();
    }

    int n_mels() const { return cfg_.n_mels; }
    bool valid() const { return is_pow2(cfg_.n_fft) && cfg_.n_mels > 0 && cfg_.hop_length > 0; }

    // Compute the log-mel of `x` (n samples). Returns row-major [n_mels, n_frames]
    // (mel-major); out_frames is set to n_frames (0 if the window is shorter than
    // one frame). Frames are non-centered: frame f spans [f*hop, f*hop + n_fft).
    std::vector<float> extract(const float* x, size_t n, int& out_frames) const {
        out_frames = 0;
        if (!valid() || !x || static_cast<int>(n) < cfg_.n_fft) return {};
        const int nf = 1 + static_cast<int>((n - cfg_.n_fft) / cfg_.hop_length);
        out_frames = nf;
        std::vector<float> mel(static_cast<size_t>(cfg_.n_mels) * nf, 0.f);
        std::vector<float> re(cfg_.n_fft), im(cfg_.n_fft), power(nbins_);
        for (int f = 0; f < nf; ++f) {
            const size_t base = static_cast<size_t>(f) * cfg_.hop_length;
            for (int i = 0; i < cfg_.n_fft; ++i) { re[i] = x[base + i] * window_[i]; im[i] = 0.f; }
            fft_pow2(re, im);
            for (int b = 0; b < nbins_; ++b) power[b] = re[b] * re[b] + im[b] * im[b];
            for (int m = 0; m < cfg_.n_mels; ++m) {
                float acc = 0.f;
                const auto& fb = filterbank_[m];
                for (int b = 0; b < nbins_; ++b) acc += fb[b] * power[b];
                const float v = cfg_.log10 ? 10.f * std::log10(acc + cfg_.log_offset)
                                           : std::log(acc + cfg_.log_offset);
                mel[static_cast<size_t>(m) * nf + f] = v;
            }
        }
        return mel;
    }

    const std::vector<std::vector<float>>& filterbank() const { return filterbank_; }

private:
    void build_filterbank() {
        filterbank_.assign(cfg_.n_mels, std::vector<float>(nbins_, 0.f));
        const float melMin = hz_to_mel(cfg_.fmin), melMax = hz_to_mel(cfg_.fmax);
        std::vector<float> hz(cfg_.n_mels + 2);
        for (int i = 0; i < cfg_.n_mels + 2; ++i)
            hz[i] = mel_to_hz(melMin + (melMax - melMin) * i / (cfg_.n_mels + 1));
        auto bin_hz = [&](int b) { return b * static_cast<float>(cfg_.sample_rate) / cfg_.n_fft; };
        for (int m = 0; m < cfg_.n_mels; ++m) {
            const float lo = hz[m], ctr = hz[m + 1], hi = hz[m + 2];
            for (int b = 0; b < nbins_; ++b) {
                const float fb_hz = bin_hz(b);
                float w = 0.f;
                if (fb_hz >= lo && fb_hz <= ctr && ctr > lo) w = (fb_hz - lo) / (ctr - lo);
                else if (fb_hz > ctr && fb_hz <= hi && hi > ctr) w = (hi - fb_hz) / (hi - ctr);
                filterbank_[m][b] = w;
            }
            if (cfg_.slaney) {
                const float enorm = 2.f / (hi_safe(hz, m) - hz[m]);
                for (int b = 0; b < nbins_; ++b) filterbank_[m][b] *= enorm;
            }
        }
    }
    static float hi_safe(const std::vector<float>& hz, int m) {
        const float d = hz[m + 2] - hz[m];
        return hz[m] + (d > 0.f ? d : 1.f);
    }

    MelConfig cfg_;
    int nbins_ = 0;
    std::vector<float> window_;
    std::vector<std::vector<float>> filterbank_;  // n_mels x nbins
};

}  // namespace zm::audio
