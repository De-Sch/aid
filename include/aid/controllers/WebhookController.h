#pragma once

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "aid/value-types/Ids.h"

namespace aid::infrastructure {
class WebhookMailbox;
class Wal;
} // namespace aid::infrastructure

namespace aid::crosscutting {
class Logger;
class CorrelationId;
} // namespace aid::crosscutting

namespace aid::controllers {

// POST /hook/ticket dispatcher (Phase 6). Same plumbing discipline as
// CallController: verify the shared secret → fsync WAL → enqueue on the
// per-ticket WebhookMailbox → 202. No business logic, no blocking; the mailbox
// worker decodes the body and emits the live delta. Registered in main.cpp only
// when a Webhook config section is present.
class WebhookController {
public:
    WebhookController(aid::infrastructure::Wal& wal, aid::infrastructure::WebhookMailbox& mailbox,
                      aid::crosscutting::Logger& logger, aid::crosscutting::CorrelationId& cid,
                      std::string secret);

    WebhookController(const WebhookController&) = delete;
    WebhookController& operator=(const WebhookController&) = delete;
    WebhookController(WebhookController&&) = delete;
    WebhookController& operator=(WebhookController&&) = delete;
    ~WebhookController() = default;

    void handlePost(const drogon::HttpRequestPtr& req,
                    std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    // Recover the affected ticket id from a webhook body — used both to key the
    // mailbox at POST time and to re-key stored records on WAL replay. Accepts
    // the ticket-system envelope ({"work_package":{"id":N,…}}) or a bare HAL work
    // package ({"id":N,…}). Empty optional when no usable id is present.
    [[nodiscard]] static std::optional<aid::TicketId> ticketIdOf(std::string_view body);

private:
    aid::infrastructure::Wal& wal_;
    aid::infrastructure::WebhookMailbox& mailbox_;
    aid::crosscutting::Logger& logger_;
    aid::crosscutting::CorrelationId& cid_;
    std::string secret_;
};

} // namespace aid::controllers
