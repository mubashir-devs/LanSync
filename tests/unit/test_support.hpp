#pragma once
// Shared test helpers.

#include <atomic>
#include <filesystem>
#include <string>

#include <unistd.h>

#include "core/common/clock.hpp"

namespace lansync::support {

// Unique temp directory removed on destruction.
class TempDir {
public:
    TempDir() {
        static std::atomic<int> counter{0};
        path_ = std::filesystem::temp_directory_path() /
                ("lansync-test-" + std::to_string(::getpid()) + "-" +
                 std::to_string(counter.fetch_add(1)));
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

class FakeClock final : public IClock {
public:
    explicit FakeClock(TimestampMs start = 1'700'000'000'000) : now_(start) {}
    [[nodiscard]] TimestampMs nowMs() const override { return now_; }
    void advance(TimestampMs ms) { now_ += ms; }

private:
    TimestampMs now_;
};

}  // namespace lansync::support
