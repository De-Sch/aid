#include "aid/infrastructure/Mailbox.h"

#include <type_traits>
#include <utility>
#include <variant>

#include "aid/crosscutting/Logger.h"
#include "aid/value-types/CallEvent.h"

namespace aid::infrastructure {

Mailbox::Mailbox(trantor::EventLoop& domainLoop, Wal& wal, aid::crosscutting::Logger& logger,
                 Handlers handlers, ReplayDecoder decoder)
    : logger_(logger), handlers_(std::move(handlers)), decoder_(std::move(decoder)),
      engine_(
          domainLoop, wal, logger, [this](Engine::Pending& p) { return dispatch(p); },
          Engine::Labels{"mailbox", "handled event callid", "usecase failed"}) {
}

aid::plumbing::Task<aid::plumbing::Result<void>> Mailbox::dispatch(Engine::Pending& p) {
    return std::visit(
        [&](auto& alt) -> aid::plumbing::Task<aid::plumbing::Result<void>> {
            using T = std::decay_t<decltype(alt)>;
            if constexpr (std::is_same_v<T, aid::IncomingCall>) {
                return handlers_.incoming(alt, p.replay);
            } else if constexpr (std::is_same_v<T, aid::OutgoingCall>) {
                return handlers_.outgoing(alt, p.replay);
            } else if constexpr (std::is_same_v<T, aid::AcceptedCall>) {
                return handlers_.accepted(alt);
            } else if constexpr (std::is_same_v<T, aid::TransferCall>) {
                return handlers_.transfer(alt);
            } else {
                static_assert(std::is_same_v<T, aid::HangupCall>,
                              "CallEvent variant has an alternative no handler knows about");
                return handlers_.hangup(alt);
            }
        },
        p.event);
}

aid::plumbing::Result<void> Mailbox::enqueue(aid::CallId callid, aid::CallEvent event,
                                             std::string correlationId, std::uint64_t walSeq) {
    return engine_.enqueue(std::move(callid), std::move(event), std::move(correlationId), walSeq,
                           /*replay=*/false);
}

void Mailbox::enqueueReplay(const aid::plumbing::WalRecord& rec) {
    if (!decoder_) {
        logger_.warn("Mailbox::enqueueReplay called without decoder; dropping record");
        return;
    }
    auto decoded = decoder_(rec.body);
    if (!decoded) {
        logger_.warn("Mailbox::enqueueReplay decoder returned nullopt; dropping record");
        return;
    }
    aid::CallId callid = aid::callidOf(*decoded);
    // replay = true: this record came from Wal::readAll on startup, so a
    // duplicate incognito create from a crash mid-truncate must be deduped by
    // the use case.
    engine_.enqueueBypass(std::move(callid), std::move(*decoded), rec.correlationId, rec.seq,
                          /*replay=*/true);
}

std::size_t Mailbox::liveCount() const {
    return engine_.liveCount();
}

std::size_t Mailbox::failedCount() const noexcept {
    return engine_.failedCount();
}

std::size_t Mailbox::trackedMailboxCount() const {
    return engine_.trackedMailboxCount();
}

void Mailbox::gcIdleOlderThan(std::chrono::seconds idle) {
    engine_.gcIdleOlderThan(idle);
}

aid::plumbing::Task<void> Mailbox::drain(std::chrono::seconds budget) {
    return engine_.drain(budget);
}

} // namespace aid::infrastructure
