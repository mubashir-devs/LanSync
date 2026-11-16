#include <gtest/gtest.h>

#include "network/protocol/framing.hpp"
#include "network/protocol/message.hpp"

using namespace lansync;
using namespace lansync::protocol;

TEST(Message, RoundTrip) {
    auto m = make(type::kHello, "abc123", {{"device_name", "x"}});
    auto parsed = parse(serialize(m));
    ASSERT_TRUE(parsed) << parsed.error().message;
    EXPECT_EQ(parsed->type, "hello");
    EXPECT_EQ(parsed->deviceId, "abc123");
    EXPECT_EQ(parsed->requestId, m.requestId);
    EXPECT_EQ(parsed->payload["device_name"], "x");
}

TEST(Message, RejectsUnsupportedVersionCleanly) {
    auto r = parse(R"({"protocol_version":99,"message_type":"hello","request_id":"r","device_id":"ab","payload":{}})");
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::UnsupportedVersion);
}

TEST(Message, RejectsMalformedInput) {
    for (const char* bad : {
             "", "garbage", "[]", "{}",
             R"({"protocol_version":1,"message_type":"nope","request_id":"r","device_id":"ab","payload":{}})",
             R"({"protocol_version":1,"message_type":"hello","request_id":"r","device_id":"XYZ","payload":{}})",
             R"({"protocol_version":1,"message_type":"hello","request_id":"r","device_id":"","payload":{}})",
             R"({"protocol_version":1,"message_type":"hello","request_id":"r","device_id":"ab","payload":[]})",
             R"({"protocol_version":"1","message_type":"hello","request_id":"r","device_id":"ab","payload":{}})",
             R"({"protocol_version":1,"message_type":5,"request_id":"r","device_id":"ab","payload":{}})"}) {
        auto r = parse(bad);
        EXPECT_FALSE(r) << bad;
    }
}

TEST(Message, RejectsOversizedIdsAndMessages) {
    const std::string longId(65, 'a');
    auto r = parse(R"({"protocol_version":1,"message_type":"hello","request_id":"r","device_id":")" +
                   longId + R"(","payload":{}})");
    EXPECT_FALSE(r);
    EXPECT_FALSE(parse(std::string(kMaxJsonBytes + 1, ' ')));
}

TEST(Framing, EncodeDecodeSingleFrame) {
    FrameDecoder d(1024);
    auto bytes = encodeJsonFrame("{\"a\":1}");
    d.feed(bytes.data(), bytes.size());
    auto f = d.next();
    ASSERT_TRUE(f);
    ASSERT_TRUE(f->has_value());
    EXPECT_EQ((*f)->kind, FrameKind::Json);
    EXPECT_EQ((*f)->payload, "{\"a\":1}");
    auto none = d.next();
    ASSERT_TRUE(none);
    EXPECT_FALSE(none->has_value());
}

TEST(Framing, ReassemblesByteByByte) {
    FrameDecoder d(1024);
    const auto a = encodeJsonFrame("first");
    const auto b = encodeChunkFrame(7, "binary\0data");
    std::string stream = a + b;
    std::vector<Frame> got;
    for (char c : stream) {
        d.feed(&c, 1);
        for (;;) {
            auto f = d.next();
            ASSERT_TRUE(f);
            if (!f->has_value()) break;
            got.push_back(std::move(**f));
        }
    }
    ASSERT_EQ(got.size(), 2u);
    EXPECT_EQ(got[0].payload, "first");
    auto chunk = parseChunk(got[1]);
    ASSERT_TRUE(chunk);
    EXPECT_EQ(chunk->streamId, 7u);
    EXPECT_EQ(chunk->data, std::string_view("binary\0data", 11).substr(0, 6));
}

TEST(Framing, MultipleFramesInOneRead) {
    FrameDecoder d(1024);
    std::string s = encodeJsonFrame("one") + encodeJsonFrame("two") + encodeJsonFrame("three");
    d.feed(s.data(), s.size());
    int n = 0;
    for (;;) {
        auto f = d.next();
        ASSERT_TRUE(f);
        if (!f->has_value()) break;
        ++n;
    }
    EXPECT_EQ(n, 3);
}

TEST(Framing, RejectsOversizedFrameFromHeaderAlone) {
    FrameDecoder d(100);
    const char header[] = {0x00, 0x10, 0x00, 0x00, 0x00};   // length 1 MiB, body not sent
    d.feed(header, sizeof(header));
    auto f = d.next();
    ASSERT_FALSE(f);
    EXPECT_EQ(f.error().code, ErrorCode::ProtocolError);
    EXPECT_FALSE(d.next());   // sticky failure
}

TEST(Framing, RejectsZeroLengthAndUnknownKind) {
    {
        FrameDecoder d(100);
        const char zero[] = {0, 0, 0, 0};
        d.feed(zero, 4);
        EXPECT_FALSE(d.next());
    }
    {
        FrameDecoder d(100);
        const char badKind[] = {0, 0, 0, 1, 9};
        d.feed(badKind, 5);
        EXPECT_FALSE(d.next());
    }
}

TEST(Framing, ChunkParseRejectsShortPayload) {
    Frame f{FrameKind::Chunk, "ab"};
    EXPECT_FALSE(parseChunk(f));
    Frame j{FrameKind::Json, "abcdef"};
    EXPECT_FALSE(parseChunk(j));
}
