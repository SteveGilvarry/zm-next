#include "webhook_util.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using zm::outputwebhook::type_allowed;

TEST(WebhookUtil, EmptyAllowListAllowsAnyType) {
    std::vector<std::string> allow;
    EXPECT_TRUE(type_allowed(allow, "motion"));
    EXPECT_TRUE(type_allowed(allow, "detection"));
    EXPECT_TRUE(type_allowed(allow, ""));
}

TEST(WebhookUtil, NonEmptyAllowsOnlyListed) {
    std::vector<std::string> allow{"motion", "detection"};
    EXPECT_TRUE(type_allowed(allow, "motion"));
    EXPECT_TRUE(type_allowed(allow, "detection"));
    EXPECT_FALSE(type_allowed(allow, "description"));
    EXPECT_FALSE(type_allowed(allow, ""));
}

TEST(WebhookUtil, CaseSensitive) {
    std::vector<std::string> allow{"motion"};
    EXPECT_TRUE(type_allowed(allow, "motion"));
    EXPECT_FALSE(type_allowed(allow, "Motion"));
    EXPECT_FALSE(type_allowed(allow, "MOTION"));
}
