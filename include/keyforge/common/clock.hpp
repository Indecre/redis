#pragma once

#include <chrono>
#include <cstdint>

namespace keyforge {

class Clock {
public:
    virtual ~Clock() = default;
    [[nodiscard]] virtual std::int64_t unix_milliseconds() const noexcept = 0;
    [[nodiscard]] virtual std::int64_t monotonic_milliseconds() const noexcept = 0;
};

class SystemClock final : public Clock {
public:
    [[nodiscard]] std::int64_t unix_milliseconds() const noexcept override {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    [[nodiscard]] std::int64_t monotonic_milliseconds() const noexcept override {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }
};

}  // namespace keyforge
