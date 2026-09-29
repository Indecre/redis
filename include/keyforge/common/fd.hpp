#pragma once

#include <unistd.h>

#include <utility>

namespace keyforge {

class Fd {
public:
    Fd() = default;
    explicit Fd(int descriptor) noexcept : descriptor_(descriptor) {}
    ~Fd() { reset(); }

    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;

    Fd(Fd&& other) noexcept : descriptor_(std::exchange(other.descriptor_, -1)) {}
    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) {
            reset(std::exchange(other.descriptor_, -1));
        }
        return *this;
    }

    [[nodiscard]] int get() const noexcept { return descriptor_; }
    [[nodiscard]] explicit operator bool() const noexcept { return descriptor_ >= 0; }

    [[nodiscard]] int release() noexcept { return std::exchange(descriptor_, -1); }

    void reset(int descriptor = -1) noexcept {
        if (descriptor_ >= 0) {
            (void)::close(descriptor_);
        }
        descriptor_ = descriptor;
    }

private:
    int descriptor_{-1};
};

}  // namespace keyforge

