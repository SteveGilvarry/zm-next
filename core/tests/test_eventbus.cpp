#include <gtest/gtest.h>
#include "zm/EventBus.hpp"
#include <vector>
#include <string>

using namespace zm;

TEST(EventBusTest, BasicSubscribePublish) {
    auto& bus = EventBus::instance();
    std::vector<std::string> rec1;
    std::vector<std::string> rec2;

    bus.subscribe("foo", [&rec1](const std::string& msg) { rec1.push_back(msg); });
    bus.subscribe("foo", [&rec2](const std::string& msg) { rec2.push_back(msg); });

    bus.publish("foo", "hello");
    bus.publish("other", "world"); // no subscribers here
    bus.publish("foo", "world");

    EXPECT_EQ(rec1.size(), 2);
    EXPECT_EQ(rec2.size(), 2);
    EXPECT_EQ(rec1[0], "hello");
    EXPECT_EQ(rec1[1], "world");
    EXPECT_EQ(rec2[0], "hello");
    EXPECT_EQ(rec2[1], "world");
}

TEST(EventBusTest, NoSubscribers) {
    auto& bus = EventBus::instance();
    // Should not crash or throw if no subscribers
    bus.publish("nobody", "nothing");
    SUCCEED();
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
