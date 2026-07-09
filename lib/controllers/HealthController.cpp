#include "aid/controllers/HealthController.h"

#include <drogon/HttpTypes.h>

#include <nlohmann/json.hpp>
#include <string>

#include "aid/infrastructure/HealthService.h"

namespace aid::controllers {

HealthController::HealthController(aid::infrastructure::HealthService& health) : health_(health) {
}

void HealthController::get(const drogon::HttpRequestPtr& req,
                           std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    (void)req;
    const auto snap = health_.current();

    // JSON keys are camelCase to match every other frontend-facing payload
    // (/ui/*). The internal Snapshot struct keeps snake_case members; this is
    // the wire-format mapping.
    nlohmann::json body = {
        {"status", snap.status},
        {"pluginsLoaded", snap.plugins_loaded},
        {"ticketSystem", snap.ticket_system},
        {"addressSystem", snap.address_system},
        {"uptimeS", snap.uptime_s},
        {"queuedEvents", snap.queued_events},
        {"failedEvents", snap.failed_events},
    };

    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(drogon::k200OK);
    // Dedicated setter, not addHeader — see ControllerSupport::jsonResponse:
    // addHeader leaves the text/html default and emits a duplicate on the wire.
    resp->setContentTypeString("application/json");
    resp->setBody(body.dump());
    callback(resp);
}

} // namespace aid::controllers
