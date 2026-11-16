#include <gtest/gtest.h>

#include <fstream>
#include <sstream>

#include "core/common/logging.hpp"
#include "test_support.hpp"

using namespace lansync;

TEST(Logging, ParsesLevels) {
    spdlog::level::level_enum lvl{};
    EXPECT_TRUE(log::parseLevel("debug", lvl));
    EXPECT_EQ(lvl, spdlog::level::debug);
    EXPECT_TRUE(log::parseLevel("off", lvl));
    EXPECT_EQ(lvl, spdlog::level::off);
    EXPECT_FALSE(log::parseLevel("verbose", lvl));
    EXPECT_EQ(lvl, spdlog::level::off);   // untouched on failure
}

TEST(Logging, WritesToFileAndHonoursLevel) {
    support::TempDir dir;
    log::LogConfig cfg;
    cfg.directory = dir.path() / "logs";
    cfg.level = "info";
    cfg.console = false;
    log::init(cfg);

    LS_DEBUG("hidden-debug-line");
    LS_INFO("visible-info-line {}", 7);
    log::shutdown();

    std::ifstream in(cfg.directory / cfg.fileName);
    ASSERT_TRUE(in.good());
    std::stringstream ss;
    ss << in.rdbuf();
    EXPECT_NE(ss.str().find("visible-info-line 7"), std::string::npos);
    EXPECT_EQ(ss.str().find("hidden-debug-line"), std::string::npos);
}

TEST(Logging, RuntimeLevelChange) {
    support::TempDir dir;
    log::LogConfig cfg;
    cfg.directory = dir.path();
    cfg.console = false;
    cfg.level = "error";
    log::init(cfg);
    EXPECT_TRUE(log::setLevel("debug"));
    EXPECT_EQ(log::get()->level(), spdlog::level::debug);
    EXPECT_FALSE(log::setLevel("nonsense"));
    EXPECT_EQ(log::get()->level(), spdlog::level::debug);
    log::shutdown();
}
