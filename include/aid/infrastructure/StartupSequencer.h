#pragma once

#include <optional>

#include "aid/plumbing/Error.h"
#include "aid/plumbing/Result.h"

namespace aid::infrastructure {

// Bootstrap-order tripwire. `main()` must drain the WAL into the mailbox
// BEFORE it opens the `/call` listener — otherwise a freshly-accepted POST could
// be processed ahead of an un-acked replayed record, breaking the at-least-once
// ordering guarantee. Until now that order was enforced only
// by the source order of the statements in main(); a careless reorder would
// silently regress it. This guard turns the contract into an explicit runtime
// check: main marks replay complete after the readAll() loop and asks the guard
// to confirm it before any addListener() call, aborting startup if the order was
// violated.
//
// Header-only: trivial state, no I/O, links only plumbing.
class StartupSequencer {
public:
    // Called once, immediately after the WAL replay loop has enqueued every
    // pending record onto the mailbox.
    void markReplayComplete() noexcept { replayComplete_ = true; }

    // Called once, immediately before listeners are opened. Returns an
    // InvariantViolation error (which main fatal-logs and aborts on) if replay
    // was not marked complete first.
    [[nodiscard]] aid::plumbing::Result<void> requireReplayedBeforeListening() const {
        if (!replayComplete_) {
            return aid::plumbing::unexpected{aid::plumbing::Error{
                aid::plumbing::ErrorCode::InvariantViolation,
                "startup ordering violated: listeners must not open before WAL replay completes",
                std::nullopt}};
        }
        return {};
    }

    [[nodiscard]] bool replayComplete() const noexcept { return replayComplete_; }

private:
    bool replayComplete_{false};
};

} // namespace aid::infrastructure
