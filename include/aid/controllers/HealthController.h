#pragma once

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include <functional>

namespace aid::infrastructure {
class HealthService;
} // namespace aid::infrastructure

namespace aid::controllers {

// HealthController — GET /health returns the cached HealthService snapshot
// as JSON. Synchronous; never issues an upstream call from the
// handler — the freshness contract is "cached at boot, live counters
// derived on every read." No auth (same trust-the-LAN policy as the
// rest of the daemon).
class HealthController {
public:
    explicit HealthController(aid::infrastructure::HealthService& health);

    HealthController(const HealthController&) = delete;
    HealthController& operator=(const HealthController&) = delete;
    HealthController(HealthController&&) = delete;
    HealthController& operator=(HealthController&&) = delete;
    ~HealthController() = default;

    void get(const drogon::HttpRequestPtr& req,
             std::function<void(const drogon::HttpResponsePtr&)>&& callback);

private:
    aid::infrastructure::HealthService& health_;
};

} // namespace aid::controllers
