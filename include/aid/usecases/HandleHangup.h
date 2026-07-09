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

// Orchestrates the Hangup event — complete the per-call comment line with the
// end-time marker, remove the callid from the active list, save. Missing
// ticket is a critical error (the one exception to the otherwise-non-fatal
// "ticket not found" treatment).
class HandleHangup {
public:
    HandleHangup(aid::ports::TicketStore& ts, aid::ports::UiNotifier& ui,
                 aid::crosscutting::Clock& clock);

    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<void>> run(const aid::HangupCall& ev);

private:
    aid::ports::TicketStore& ts_;
    aid::ports::UiNotifier& ui_;
    aid::crosscutting::Clock& clock_;
};

} // namespace aid::usecases
