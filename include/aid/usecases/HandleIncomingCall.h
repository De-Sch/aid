#pragma once

#include "aid/crosscutting/Config.h"
#include "aid/plumbing/Result.h"
#include "aid/plumbing/Task.h"
#include "aid/value-types/CallEvent.h"

namespace aid::ports {
class TicketStore;
class AddressBook;
class UiNotifier;
} // namespace aid::ports

namespace aid::crosscutting {
class Clock;
} // namespace aid::crosscutting

namespace aid::usecases {

// Orchestrates the Ring procedure for an incoming-call event. Pure
// orchestration: no JSON, no HTTP, no Drogon. Errors propagate silently —
// the mailbox worker boundary logs them and decides on WAL truncation.
class HandleIncomingCall {
public:
    HandleIncomingCall(aid::ports::TicketStore& ts, aid::ports::AddressBook& ab,
                       aid::ports::UiNotifier& ui, aid::crosscutting::Clock& clock,
                       const aid::crosscutting::Config::TicketRouting& cfg);

    // `replay` is true when the event is being re-dispatched from the WAL on
    // startup. Only the incognito branch reads it: a live
    // incognito call always opens a fresh ticket, but a replayed one
    // must be deduped by exact callid so a crash mid-WAL-truncate cannot
    // double-create. The routable branches self-dedup by caller number and so
    // ignore it. Defaults to false (the live path).
    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<void>> run(const aid::IncomingCall& ev,
                                                                       bool replay = false);

private:
    aid::ports::TicketStore& ts_;
    aid::ports::AddressBook& ab_;
    aid::ports::UiNotifier& ui_;
    aid::crosscutting::Clock& clock_;
    const aid::crosscutting::Config::TicketRouting& cfg_;
};

} // namespace aid::usecases
