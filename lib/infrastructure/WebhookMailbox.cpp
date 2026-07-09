#include "aid/infrastructure/WebhookMailbox.h"

#include <utility>

#include "aid/crosscutting/Logger.h"

namespace aid::infrastructure {

WebhookMailbox::WebhookMailbox(trantor::EventLoop& domainLoop, Wal& wal,
                               aid::crosscutting::Logger& logger, Handler handler,
                               KeyExtractor extractor)
    : logger_(logger), handler_(std::move(handler)), extractor_(std::move(extractor)),
      engine_(
          domainLoop, wal, logger, [this](Engine::Pending& p) { return dispatch(p); },
          Engine::Labels{"webhook mailbox", "handled ticket", "handler failed"}) {
}

aid::plumbing::Task<aid::plumbing::Result<void>> WebhookMailbox::dispatch(Engine::Pending& p) {
    return handler_(std::move(p.event), p.correlationId);
}

aid::plumbing::Result<void> WebhookMailbox::enqueue(aid::TicketId ticketId, std::string payload,
                                                    std::string correlationId,
                                                    std::uint64_t walSeq) {
    return engine_.enqueue(std::move(ticketId), std::move(payload), std::move(correlationId),
                           walSeq, /*replay=*/false);
}

void WebhookMailbox::enqueueReplay(const aid::plumbing::WalRecord& rec) {
    if (!extractor_) {
        logger_.warn("WebhookMailbox::enqueueReplay called without extractor; dropping record");
        return;
    }
    auto key = extractor_(rec.body);
    if (!key) {
        logger_.warn("WebhookMailbox::enqueueReplay could not key record; dropping");
        return;
    }
    engine_.enqueueBypass(std::move(*key), rec.body, rec.correlationId, rec.seq, /*replay=*/false);
}

std::size_t WebhookMailbox::liveCount() const {
    return engine_.liveCount();
}

std::size_t WebhookMailbox::failedCount() const noexcept {
    return engine_.failedCount();
}

std::size_t WebhookMailbox::trackedMailboxCount() const {
    return engine_.trackedMailboxCount();
}

void WebhookMailbox::gcIdleOlderThan(std::chrono::seconds idle) {
    engine_.gcIdleOlderThan(idle);
}

aid::plumbing::Task<void> WebhookMailbox::drain(std::chrono::seconds budget) {
    return engine_.drain(budget);
}

} // namespace aid::infrastructure
