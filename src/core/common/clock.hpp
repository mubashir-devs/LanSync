#pragma once
// Injectable time source so time-dependent logic (retention, timeouts,
// conflict naming) is deterministic under test.

#include <chrono>
#include <cstdint>

namespace lansync {

// UTC milliseconds since the Unix epoch - the unit used in all DB timestamps.
using TimestampMs = std::int64_t;

class IClock {
public:
    virtual ~IClock() = default;
    [[nodiscard]] virtual TimestampMs nowMs() const = 0;
};

class SystemClock final : public IClock {
public:
    [[nodiscard]] TimestampMs nowMs() const override {
        using namespace std::chrono;
        return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
    }
};

}  // namespace lansync
