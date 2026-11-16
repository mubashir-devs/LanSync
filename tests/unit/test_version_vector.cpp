#include <gtest/gtest.h>

#include "core/sync/version_vector.hpp"

using namespace lansync::sync;
using Order = VersionVector::Order;

namespace {
VersionVector vv(std::initializer_list<std::pair<const char*, int>> l) {
    VersionVector v;
    for (auto [k, n] : l) for (int i = 0; i < n; ++i) v.bump(k);
    return v;
}
}  // namespace

TEST(VersionVector, Comparison) {
    EXPECT_EQ(vv({{"aa", 1}}).compare(vv({{"aa", 1}})), Order::Equal);
    EXPECT_EQ(vv({}).compare(vv({})), Order::Equal);
    EXPECT_EQ(vv({{"aa", 2}}).compare(vv({{"aa", 1}})), Order::Greater);
    EXPECT_EQ(vv({{"aa", 1}}).compare(vv({{"aa", 1}, {"bb", 1}})), Order::Less);
    EXPECT_EQ(vv({{"aa", 2}}).compare(vv({{"aa", 1}, {"bb", 1}})), Order::Concurrent);
}

TEST(VersionVector, MergeIsComponentWiseMaxAndDominatesBoth) {
    auto a = vv({{"aa", 2}});
    auto b = vv({{"aa", 1}, {"bb", 3}});
    auto m = a;
    m.merge(b);
    EXPECT_EQ(m.get("aa"), 2u);
    EXPECT_EQ(m.get("bb"), 3u);
    EXPECT_NE(m.compare(a), Order::Less);
    EXPECT_NE(m.compare(b), Order::Less);
}

TEST(VersionVector, JsonRoundTrip) {
    auto a = vv({{"aa", 2}, {"bb", 1}});
    auto back = VersionVector::fromJson(a.toJson());
    ASSERT_TRUE(back);
    EXPECT_TRUE(*back == a);
    EXPECT_TRUE(VersionVector::fromString(a.toString()) == a);
}

TEST(VersionVector, RejectsHostileJson) {
    using nlohmann::json;
    EXPECT_FALSE(VersionVector::fromJson(json::array()));
    EXPECT_FALSE(VersionVector::fromJson(json{{"NOTHEX", 1}}));
    EXPECT_FALSE(VersionVector::fromJson(json{{"aa", -1}}));
    EXPECT_FALSE(VersionVector::fromJson(json{{"aa", "x"}}));
    json big = json::object();
    for (int i = 0; i < 65; ++i) big[std::to_string(i) + "a"] = 1;   // not hex for some, too many anyway
    EXPECT_FALSE(VersionVector::fromJson(big));
    EXPECT_TRUE(VersionVector::fromString("garbage").empty());
}
