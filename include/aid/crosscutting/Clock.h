#pragma once

#include <chrono>

#include "aid/value-types/Ids.h"

namespace aid::crosscutting {

class Clock {
public:
    Clock() = default;
    Clock(const Clock&) = delete;
    Clock& operator=(const Clock&) = delete;
    Clock(Clock&&) = delete;
    Clock& operator=(Clock&&) = delete;
    virtual ~Clock() = default;

    [[nodiscard]] virtual Timestamp now() const = 0;
};

class RealClock final : public Clock {
public:
    // Trivial one-liner; kept inline so RealClock needs no translation unit.
    [[nodiscard]] Timestamp now() const override { return std::chrono::system_clock::now(); }
};

} // namespace aid::crosscutting
