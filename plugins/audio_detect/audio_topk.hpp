// audio_topk.hpp — pure top-k helper for audio classifier scores.
//
// No FFmpeg/ORT/JSON dependencies: this is deliberately dependency-free so it
// can be unit tested in isolation. Given a contiguous array of class scores it
// returns up to `top_k` (index, score) pairs whose score is >= conf_threshold,
// sorted by score descending (ties broken by ascending index for determinism).

#pragma once

#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

namespace zm {
namespace audio {

// Returns the top-k (index, score) pairs above conf_threshold, sorted by score
// descending. If top_k <= 0 or count == 0 or scores == nullptr, returns empty.
inline std::vector<std::pair<int, float>>
top_k_above_threshold(const float* scores, std::size_t count,
                      float conf_threshold, int top_k) {
    std::vector<std::pair<int, float>> result;
    if (!scores || count == 0 || top_k <= 0) {
        return result;
    }

    // Collect everything at/above threshold.
    result.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        if (scores[i] >= conf_threshold) {
            result.emplace_back(static_cast<int>(i), scores[i]);
        }
    }

    // Sort by score descending; break ties by ascending index for determinism.
    std::sort(result.begin(), result.end(),
              [](const std::pair<int, float>& a, const std::pair<int, float>& b) {
                  if (a.second != b.second) return a.second > b.second;
                  return a.first < b.first;
              });

    // Cap to top_k.
    if (result.size() > static_cast<std::size_t>(top_k)) {
        result.resize(static_cast<std::size_t>(top_k));
    }
    return result;
}

} // namespace audio
} // namespace zm
