#include <gtest/gtest.h>

#include "core/common/result.hpp"

using lansync::Error;
using lansync::ErrorCode;
using lansync::Result;

TEST(Result, HoldsValue) {
    Result<int> r = 42;
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(*r, 42);
}

TEST(Result, HoldsError) {
    Result<int> r = Error{ErrorCode::NotFound, "nope"};
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::NotFound);
    EXPECT_EQ(r.error().message, "nope");
}

TEST(Result, VoidSpecialisation) {
    Result<void> ok;
    EXPECT_TRUE(ok);
    Result<void> bad = Error{ErrorCode::IoError, "disk"};
    EXPECT_FALSE(bad);
    EXPECT_EQ(bad.error().code, ErrorCode::IoError);
}
