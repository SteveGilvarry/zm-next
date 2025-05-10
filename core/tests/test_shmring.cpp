#include <gtest/gtest.h>
#include "zm/ShmRing.hpp"
#include <vector>

using namespace zm;

TEST(ShmRingTest, PushPopBasic) {
    const size_t slotCount = 4;
    const size_t slotSize = 16;

    ShmRing ring(slotCount, slotSize);
    std::vector<char> data(slotSize, 0x5A);
    std::vector<char> out(slotSize, 0);
    size_t outSize = 0;

    // Push slotCount - 1 items
    for (size_t i = 0; i < slotCount - 1; ++i) {
        data[0] = static_cast<char>(i);
        EXPECT_TRUE(ring.push(data.data(), data.size()));
    }

    // Ring should be full now: next push fails
    EXPECT_FALSE(ring.push(data.data(), data.size()));

    // Pop all items
    for (size_t i = 0; i < slotCount - 1; ++i) {
        EXPECT_TRUE(ring.pop(out.data(), outSize));
        EXPECT_EQ(outSize, slotSize);
        EXPECT_EQ(out[0], static_cast<char>(i));
    }

    // Now empty: pop blocks then should return true once push occurs
    // But blocking behavior is not tested here
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
