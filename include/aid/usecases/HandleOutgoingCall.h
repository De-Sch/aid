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

// Orchestrates the outgoing-call flow. Same shape as HandleIncomingCall
// with two deltas: user is resolved first (nullopt → non-fatal early
// return), and the assignee is set on the ticket (reuse or
// create). OutgoingCall has no `dialed`; calledNumber stays nullopt.
class HandleOutgoingCall {
public:
    HandleOutgoingCall(aid::ports::TicketStore& ts, aid::ports::AddressBook& ab,
                       aid::ports::UiNotifier& ui, aid::crosscutting::Clock& clock,
                       const aid::crosscutting::Config::TicketRouting& cfg);

    // `replay` is true when the event is being re-dispatched from the WAL on
    // startup; see HandleIncomingCall::run. Only the
    // incognito branch reads it, to dedup a replayed create by exact callid.
    // Defaults to false (the live path).
    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<void>> run(const aid::OutgoingCall& ev,
                                                                       bool replay = false);

private:
    aid::ports::TicketStore& ts_;
    aid::ports::AddressBook& ab_;
    aid::ports::UiNotifier& ui_;
    aid::crosscutting::Clock& clock_;
    const aid::crosscutting::Config::TicketRouting& cfg_;
};

} // namespace aid::usecases
