#include "zm/StageRunner.hpp"
#include "zm_plugin.h"

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace zm;

namespace {
std::atomic<int> g_fast{0};
void fast_on_frame(zm_plugin_t*, const void*, size_t) { g_fast.fetch_add(1); }
void slow_on_frame(zm_plugin_t*, const void*, size_t) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
}
void noop_on_frame(zm_plugin_t*, const void*, size_t) {}

std::vector<uint8_t> frame() { return std::vector<uint8_t>(sizeof(zm_frame_hdr_t) + 8, 0); }
}  // namespace

TEST(StageRunnerTest, ProcessesAllWhenFastEnough) {
    g_fast = 0;
    zm_plugin_t p{};
    p.on_frame = fast_on_frame;
    StageRunner r(&p, /*max_depth=*/128);
    r.start();
    auto f = frame();
    for (int i = 0; i < 50; ++i) r.deliver(f.data(), f.size());
    for (int i = 0; i < 200 && r.processed() < 50; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    r.stop();
    EXPECT_EQ(r.processed(), 50u);
    EXPECT_EQ(r.dropped(), 0u);
}

TEST(StageRunnerTest, SlowStageDropsOldestAndNeverBlocks) {
    zm_plugin_t p{};
    p.on_frame = slow_on_frame;  // 20ms each — far slower than delivery
    StageRunner r(&p, /*max_depth=*/4);
    r.start();
    auto f = frame();
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 100; ++i) r.deliver(f.data(), f.size());
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0).count();
    // 100 deliveries must not block on the slow consumer (would be ~2000ms if it did).
    EXPECT_LT(ms, 300);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    r.stop();
    EXPECT_GT(r.dropped(), 0u);  // backlog beyond depth 4 was dropped
}

TEST(StageRunnerTest, ForwardsToChildStage) {
    g_fast = 0;
    zm_plugin_t child{};
    child.on_frame = fast_on_frame;
    StageRunner childRunner(&child, 64);
    childRunner.start();

    zm_plugin_t parent{};
    parent.on_frame = noop_on_frame;
    StageRunner parentRunner(&parent, 64);
    parentRunner.setChildren({&childRunner});

    auto f = frame();
    parentRunner.forwardToChildren(f.data(), f.size());
    for (int i = 0; i < 200 && childRunner.processed() < 1; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    childRunner.stop();
    EXPECT_EQ(childRunner.processed(), 1u);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
