#pragma once

#include <chrono>

#include "aid/crosscutting/Clock.h"
#include "aid/value-types/Ids.h"

namespace aid::fakes {

// Header-only test Clock with a settable wall-clock time. Consolidates the two
// former variants: the minimal aid::fakes::FakeClock (public `now_`, assigned
// directly by the use-case/integration tests) and the richer
// aid::auth::testing::FakeClock (ctor + set() + advance(), used by the auth /
// session tests). Both surfaces are kept so every existing call site compiles
// unchanged — `now_` stays public for the direct-assignment sites.
class FakeClock final : public aid::crosscutting::Clock {
public:
    aid::Timestamp now_{};

    FakeClock() noexcept = default;
    explicit FakeClock(aid::Timestamp t) noexcept : now_(t) {}

    [[nodiscard]] aid::Timestamp now() const override { return now_; }

    void set(aid::Timestamp t) noexcept { now_ = t; }
    void advance(std::chrono::seconds delta) noexcept { now_ += delta; }
};

} // namespace aid::fakes
