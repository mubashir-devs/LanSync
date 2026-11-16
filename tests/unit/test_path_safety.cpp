#include <gtest/gtest.h>

#include <cstring>
#include <fstream>

#include "core/common/path_safety.hpp"
#include "test_support.hpp"

using namespace lansync;
using namespace lansync::pathsafety;

TEST(PathSafety, AcceptsOrdinaryRelativePaths) {
    for (const char* ok : {"a", "a/b/c.txt", "dir with spaces/файл.txt", ".hidden", "a.b/c..d", "a/..b"}) {
        EXPECT_TRUE(validateRelativePath(ok)) << ok;
    }
}

TEST(PathSafety, RejectsTraversalAbsoluteAndMalformed) {
    for (const char* bad : {"", "/etc/passwd", "../x", "a/../../x", "a/..", "..", ".", "a/./b", "a//b",
                            "a/", "C:/windows", "C:foo", "a\\b", ".lansync", ".lansync/tmp/x"}) {
        EXPECT_FALSE(validateRelativePath(std::string_view(bad, std::strlen(bad)))) << bad;
    }
    EXPECT_FALSE(validateRelativePath(std::string_view("a\0b", 3)));
    EXPECT_FALSE(validateRelativePath("a\nb"));
    EXPECT_FALSE(validateRelativePath(std::string(kMaxPathBytes + 1, 'a')));
    EXPECT_FALSE(validateRelativePath(std::string(kMaxComponentBytes + 1, 'a')));
    EXPECT_TRUE(validateRelativePath(std::string(kMaxComponentBytes, 'a')));
}

TEST(PathSafety, InternalPathDetection) {
    EXPECT_TRUE(isInternalPath(".lansync"));
    EXPECT_TRUE(isInternalPath(".lansync/tmp/x"));
    EXPECT_FALSE(isInternalPath(".lansyncfoo"));
    EXPECT_FALSE(isInternalPath("a/.lansync"));
}

TEST(PathSafety, ResolveInsideStaysWithinRoot) {
    support::TempDir dir;
    std::filesystem::create_directories(dir.path() / "root" / "sub");
    auto r = resolveInside(dir.path() / "root", "sub/file.txt");
    ASSERT_TRUE(r) << r.error().message;
    EXPECT_EQ(r->filename(), "file.txt");
    EXPECT_FALSE(resolveInside(dir.path() / "root", "../outside"));
}

TEST(PathSafety, SymlinkAncestorsAreRefused) {
    support::TempDir dir;
    const auto root = dir.path() / "root";
    const auto outside = dir.path() / "outside";
    std::filesystem::create_directories(root);
    std::filesystem::create_directories(outside);
    std::filesystem::create_directory_symlink(outside, root / "link");
    auto r = resolveInside(root, "link/evil.txt");
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::SecurityError);
}

TEST(PathSafety, SiblingWithSharedPrefixIsNotInside) {
    // "/x/ab" must not be accepted as inside "/x/a".
    support::TempDir dir;
    std::filesystem::create_directories(dir.path() / "a");
    std::filesystem::create_directories(dir.path() / "ab");
    // Only reachable via traversal, which validate already rejects.
    EXPECT_FALSE(resolveInside(dir.path() / "a", "../ab/f"));
}

TEST(PathSafety, ToRelativeUsesForwardSlashes) {
    EXPECT_EQ(toRelative("/r", "/r/a/b.txt"), "a/b.txt");
}
