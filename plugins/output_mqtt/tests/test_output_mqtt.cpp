#include "mqtt_topic.hpp"

#include <gtest/gtest.h>

using zm::outputmqtt::topic_for;

TEST(OutputMqttTopic, AppendsType) {
    EXPECT_EQ(topic_for("zm-next", "motion"), "zm-next/motion");
    EXPECT_EQ(topic_for("zm-next", "detection"), "zm-next/detection");
}

TEST(OutputMqttTopic, FallsBackWhenTypeEmpty) {
    EXPECT_EQ(topic_for("zm-next", ""), "zm-next/event");
}

TEST(OutputMqttTopic, HonoursCustomBase) {
    EXPECT_EQ(topic_for("home/cams", "motion"), "home/cams/motion");
    EXPECT_EQ(topic_for("home/cams", ""), "home/cams/event");
}
