#include "core/common/logging.hpp"

#include <vector>

#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

namespace lansync::log {

namespace {
constexpr const char* kLoggerName = "lansync";
}

bool parseLevel(std::string_view name, spdlog::level::level_enum& out) {
    using spdlog::level::level_enum;
    struct Entry { std::string_view name; level_enum level; };
    static constexpr Entry kLevels[] = {
        {"trace", level_enum::trace}, {"debug", level_enum::debug},
        {"info", level_enum::info},   {"warn", level_enum::warn},
        {"error", level_enum::err},   {"critical", level_enum::critical},
        {"off", level_enum::off},
    };
    for (const auto& e : kLevels) {
        if (e.name == name) {
            out = e.level;
            return true;
        }
    }
    return false;
}

void init(const LogConfig& config) {
    std::vector<spdlog::sink_ptr> sinks;
    if (config.console) {
        sinks.push_back(std::make_shared<spdlog::sinks::stderr_color_sink_mt>());
    }
    if (!config.directory.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(config.directory, ec);
        sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            (config.directory / config.fileName).string(), config.maxFileSizeBytes,
            config.maxFiles));
    }

    auto logger = std::make_shared<spdlog::logger>(kLoggerName, sinks.begin(), sinks.end());
    logger->set_pattern("%Y-%m-%d %H:%M:%S.%e [%l] [t%t] %v");

    spdlog::level::level_enum level = spdlog::level::info;
    if (!parseLevel(config.level, level)) {
        logger->warn("Unknown log level '{}', using 'info'", config.level);
    }
    logger->set_level(level);
    logger->flush_on(spdlog::level::warn);

    spdlog::drop(kLoggerName);
    spdlog::register_logger(logger);
}

bool setLevel(std::string_view name) {
    spdlog::level::level_enum level{};
    if (!parseLevel(name, level)) return false;
    get()->set_level(level);
    return true;
}

void shutdown() {
    if (auto logger = spdlog::get(kLoggerName)) logger->flush();
    spdlog::drop(kLoggerName);
}

std::shared_ptr<spdlog::logger> get() {
    // Before init() (or in tests) fall back to a console logger rather than crash.
    if (auto logger = spdlog::get(kLoggerName)) return logger;
    auto fallback = spdlog::stderr_color_mt(kLoggerName);
    return fallback;
}

}  // namespace lansync::log
