#include <gtest/gtest.h>
#include "zm/monitor.hpp"

using namespace zm;

TEST(MonitorTest, NoPipelineConfigured) {
    // Expect startMonitor to return gracefully when no pipeline is set
    EXPECT_NO_THROW(startMonitor(12345));
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
