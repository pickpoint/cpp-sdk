#include <gtest/gtest.h>

#include "pickpoint/config.hpp"
#include "pickpoint/errors.hpp"

TEST(Config, Defaults) {
  EXPECT_STREQ(pickpoint::kDefaultBaseUrl, "https://api.pickpoint.io");
  EXPECT_EQ(pickpoint::kDefaultMaxRetries, 3);
  EXPECT_EQ(pickpoint::kDefaultConcurrency, 20);
}

TEST(Errors, ApiErrorFlags) {
  pickpoint::ApiError auth(401, "API_AUTH", "nope");
  EXPECT_TRUE(auth.is_auth());
  pickpoint::ApiError nf(404, "NOT_FOUND", "missing");
  EXPECT_TRUE(nf.is_not_found());
  pickpoint::ApiError conflict(409, "CONFLICT", "dup");
  EXPECT_TRUE(conflict.is_conflict());
}
