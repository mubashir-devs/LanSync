#pragma once
// Thin wrapper around spdlog. One named logger ("lansync") with a rotating
// file sink and an optional console sink. Modules log via the LS_* macros.

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include <spdlog/spdlog.h>

namespace lansync::log {

struct LogConfig {
    std::filesystem::path directory;      // empty => no file sink
    std::string fileName{"lansync.log"};
    std::string level{"info"};            // trace|debug|info|warn|error|critical|off
    bool console{true};
    std::size_t maxFileSizeBytes{5 * 1024 * 1024};
    std::size_t maxFiles{3};
};

// Parses a level name; returns false (and leaves `out` untouched) if invalid.
bool parseLevel(std::string_view name, spdlog::level::level_enum& out);

// Creates/replaces the global "lansync" logger. Throws spdlog::spdlog_ex if the
// log file cannot be opened.
void init(const LogConfig& config);

// Changes verbosity at runtime. Returns false for an invalid level name.
bool setLevel(std::string_view name);

// Flushes and releases sinks.
void shutdown();

std::shared_ptr<spdlog::logger> get();

}  // namespace lansync::log

#define LS_TRACE(...) ::lansync::log::get()->trace(__VA_ARGS__)
#define LS_DEBUG(...) ::lansync::log::get()->debug(__VA_ARGS__)
#define LS_INFO(...) ::lansync::log::get()->info(__VA_ARGS__)
#define LS_WARN(...) ::lansync::log::get()->warn(__VA_ARGS__)
#define LS_ERROR(...) ::lansync::log::get()->error(__VA_ARGS__)
#define LS_CRITICAL(...) ::lansync::log::get()->critical(__VA_ARGS__)
