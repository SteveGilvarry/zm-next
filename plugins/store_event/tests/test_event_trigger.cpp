#include "event_trigger.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using zm::storeevent::is_trigger;

TEST(EventTrigger, MatchesExplicitList) {
    std::vector<std::string> types = {"motion", "detection"};
    EXPECT_TRUE(is_trigger(types, "motion"));
    EXPECT_TRUE(is_trigger(types, "detection"));
    EXPECT_FALSE(is_trigger(types, "audio_event"));
    EXPECT_FALSE(is_trigger(types, "tracked_detection"));
}

TEST(EventTrigger, EmptyTypeNeverTriggers) {
    std::vector<std::string> types = {"motion"};
    EXPECT_FALSE(is_trigger(types, ""));
    std::vector<std::string> empty;
    EXPECT_FALSE(is_trigger(empty, ""));
}

TEST(EventTrigger, EmptyListFallsBackToDefaults) {
    std::vector<std::string> empty;
    EXPECT_TRUE(is_trigger(empty, "motion"));
    EXPECT_TRUE(is_trigger(empty, "detection"));
    EXPECT_TRUE(is_trigger(empty, "audio_event"));
    EXPECT_TRUE(is_trigger(empty, "tracked_detection"));
    EXPECT_FALSE(is_trigger(empty, "something_else"));
}

TEST(EventTrigger, ExplicitListOverridesDefaults) {
    // An explicit list that omits a default means that default no longer fires.
    std::vector<std::string> types = {"custom_event"};
    EXPECT_TRUE(is_trigger(types, "custom_event"));
    EXPECT_FALSE(is_trigger(types, "motion"));
}

TEST(EventTrigger, DisableViaNonMatchingList) {
    // Documented way to disable triggering: a list with no real event types.
    std::vector<std::string> types = {"none"};
    EXPECT_FALSE(is_trigger(types, "motion"));
    EXPECT_FALSE(is_trigger(types, "detection"));
    EXPECT_TRUE(is_trigger(types, "none"));  // only the literal "none" matches
}
