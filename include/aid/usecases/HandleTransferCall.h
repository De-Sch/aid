#pragma once

#include "aid/plumbing/Result.h"
#include "aid/plumbing/Task.h"
#include "aid/value-types/CallEvent.h"

namespace aid::ports {
class TicketStore;
class UiNotifier;
} // namespace aid::ports

namespace aid::usecases {

// Orchestrates the Transfer event — rewrite the per-call comment line to
// show the new assignee and update ticket.assignee. Lookup by callid
// substring (a transfer on a multi-call ticket).
class HandleTransferCall {
public:
    HandleTransferCall(aid::ports::TicketStore& ts, aid::ports::UiNotifier& ui);

    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<void>> run(const aid::TransferCall& ev);

private:
    aid::ports::TicketStore& ts_;
    aid::ports::UiNotifier& ui_;
};

} // namespace aid::usecases
