#include <gtest/gtest.h>

#include <fstream>

#include "core/common/crypto.hpp"
#include "test_support.hpp"

using namespace lansync;

TEST(Crypto, Sha256KnownVectors) {
    EXPECT_EQ(crypto::sha256Hex(""),
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(crypto::sha256Hex("abc"),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(Crypto, IncrementalEqualsOneShot) {
    crypto::Sha256 h;
    h.update("a");
    h.update("bc");
    EXPECT_EQ(h.finishHex(), crypto::sha256Hex("abc"));
}

TEST(Crypto, HexRoundTripAndValidation) {
    auto bytes = crypto::fromHex("00ff10Ab");
    ASSERT_TRUE(bytes);
    EXPECT_EQ(crypto::toHex(*bytes), "00ff10ab");
    EXPECT_FALSE(crypto::fromHex("abc"));    // odd length
    EXPECT_FALSE(crypto::fromHex("zz"));
    EXPECT_TRUE(crypto::isLowerHex("09af"));
    EXPECT_FALSE(crypto::isLowerHex("09AF"));
}

TEST(Crypto, HashFileStreamsAndMatchesInMemoryHash) {
    support::TempDir dir;
    const auto p = dir.path() / "f.bin";
    std::string content(3 * 1024 * 1024 + 17, 'x');
    for (std::size_t i = 0; i < content.size(); i += 97) content[i] = static_cast<char>(i);
    { std::ofstream(p, std::ios::binary) << content; }
    auto h = crypto::hashFile(p, 4096);   // small buffer forces many reads
    ASSERT_TRUE(h);
    EXPECT_EQ(*h, crypto::sha256Hex(content));
}

TEST(Crypto, HashFileMissingIsError) {
    EXPECT_FALSE(crypto::hashFile("/nonexistent/file"));
}

TEST(Crypto, HashFileHonoursCancel) {
    support::TempDir dir;
    const auto p = dir.path() / "f";
    { std::ofstream(p) << "data"; }
    std::atomic<bool> cancel{true};
    auto h = crypto::hashFile(p, 4096, &cancel);
    ASSERT_FALSE(h);
    EXPECT_EQ(h.error().code, ErrorCode::Cancelled);
}

TEST(Crypto, PrefixHashAllowsResume) {
    support::TempDir dir;
    const auto p = dir.path() / "f";
    const std::string content = "0123456789abcdefghij";
    { std::ofstream(p, std::ios::binary) << content.substr(0, 12); }   // partial file
    crypto::Sha256 h;
    auto n = crypto::hashFilePrefix(p, 12, h);
    ASSERT_TRUE(n);
    EXPECT_EQ(*n, 12u);
    h.update(content.substr(12));
    EXPECT_EQ(h.finishHex(), crypto::sha256Hex(content));
}

TEST(Crypto, RandomHexIsRightLengthAndDiffers) {
    auto a = crypto::randomHex(16);
    auto b = crypto::randomHex(16);
    EXPECT_EQ(a.size(), 32u);
    EXPECT_NE(a, b);
    EXPECT_TRUE(crypto::isLowerHex(a));
}
