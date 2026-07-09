#pragma once

#include "aid/plumbing/Result.h"
#include "aid/plumbing/Task.h"
#include "aid/value-types/CallEvent.h"

namespace aid::ports {
class TicketStore;
class UiNotifier;
} // namespace aid::ports

namespace aid::crosscutting {
class Clock;
} // namespace aid::crosscutting

namespace aid::usecases {

// Orchestrates the Accepted event — mark ticket in-progress, stamp callStart
// on every accept (most recent call's start; per-call history is kept in
// callLength), append the open `Call start:` line per call. Pure
// orchestration: no JSON, no HTTP, no Drogon. Errors propagate silently —
// the mailbox worker boundary logs them.
class HandleAcceptedCall {
public:
    HandleAcceptedCall(aid::ports::TicketStore& ts, aid::ports::UiNotifier& ui,
                       aid::crosscutting::Clock& clock);

    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<void>> run(const aid::AcceptedCall& ev);

private:
    aid::ports::TicketStore& ts_;
    aid::ports::UiNotifier& ui_;
    aid::crosscutting::Clock& clock_;
};

} // namespace aid::usecases
