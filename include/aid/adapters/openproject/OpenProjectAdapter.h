#pragma once

// OpenProjectAdapter — concrete TicketStore implementation.
//
// This is the public class the plugin .so exposes. The extern "C"
// factory triplet (create_TicketStore / destroy_TicketStore /
// aid_plugin_api_version) is defined in OpenProjectAdapter.cpp.
//
// The class is intentionally trivial composition: every TicketStore
// virtual forwards to the matching helper (OpUserRepo / OpTicketRepo /
// OpDashboardBuilder). Routing, dashboard, and lockVersion logic each
// live in their own helper file.

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "aid/adapters/openproject/internal/CustomFieldMap.h"
#include "aid/adapters/openproject/internal/HandlerLedger.h"
#include "aid/adapters/openproject/internal/HttpDispatcher.h"
#include "aid/adapters/openproject/internal/OpDashboardBuilder.h"
#include "aid/adapters/openproject/internal/OpHttp.h"
#include "aid/adapters/openproject/internal/OpStatusMap.h"
#include "aid/adapters/openproject/internal/OpTicketRepo.h"
#include "aid/adapters/openproject/internal/OpUserRepo.h"
#include "aid/adapters/openproject/internal/ProducedLedger.h"
#include "aid/crosscutting/Config.h"
#include "aid/infrastructure/HttpClient.h"
#include "aid/plumbing/Result.h"
#include "aid/plumbing/Task.h"
#include "aid/ports/TicketStore.h"
#include "aid/value-types/Dashboard.h"
#include "aid/value-types/Ids.h"
#include "aid/value-types/MembershipDelta.h"
#include "aid/value-types/Ticket.h"
#include "aid/value-types/WebhookDecode.h"

namespace aid::adapters::openproject {

class OpenProjectAdapter final : public aid::ports::TicketStore {
public:
    // Built by the extern "C" factory after parsing config_json and
    // wiring HttpClient + Sleeper from the event loop.
    OpenProjectAdapter(std::unique_ptr<aid::infrastructure::HttpClient> http,
                       aid::crosscutting::TicketSystemConfig opCfg,
                       aid::crosscutting::UiConfig uiCfg, CustomFieldMap fields, Sleeper sleeper);

    ~OpenProjectAdapter() override = default;

    OpenProjectAdapter(const OpenProjectAdapter&) = delete;
    OpenProjectAdapter& operator=(const OpenProjectAdapter&) = delete;
    OpenProjectAdapter(OpenProjectAdapter&&) = delete;
    OpenProjectAdapter& operator=(OpenProjectAdapter&&) = delete;

    // ─── TicketStore — every virtual forwards to a helper ───────────────

    // Shutdown hook: forwards to the shared HttpClient's
    // cancellation so any worker suspended in an OpenProject co_await unwinds
    // promptly during the graceful drain.
    void cancelPendingRequests() noexcept override;

    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<aid::Ticket>>
    fetchById(aid::TicketId id) override;

    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<std::optional<aid::Ticket>>>
    findByExactCallid(aid::CallId id) override;

    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<std::optional<aid::Ticket>>>
    findByCallidContains(aid::CallId id) override;

    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<std::optional<aid::Ticket>>>
    findLatestOpenCallInProject(aid::ProjectId project) override;

    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<std::optional<aid::Ticket>>>
    findOpenInProjectBySubject(aid::ProjectId project, std::string_view subject) override;

    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<std::optional<aid::Ticket>>>
    findOpenInProjectByCallerNumber(aid::ProjectId project, aid::PhoneNumber caller) override;

    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<std::optional<aid::UserHandle>>>
    resolveUser(std::string_view login) override;

    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<std::vector<aid::ProjectId>>>
    listProjectsForUser(aid::UserHandle user) override;

    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<std::vector<aid::DashboardEntry>>>
    listDashboard(aid::UserHandle viewer) override;

    [[nodiscard]] aid::DashboardEntry buildEntry(const aid::Ticket& ticket,
                                                 aid::UserHandle viewer) override;

    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<aid::TicketId>>
    create(const aid::NewTicket& ticket) override;

    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<aid::Ticket>>
    save(aid::TicketId id, aid::ports::TicketReducer reduce) override;

    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<void>>
    addCallHandler(aid::TicketId id, aid::UserHandle login) override;

    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<std::vector<aid::UserHandle>>>
    recipientsFor(const aid::Ticket& ticket) override;

    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<std::vector<aid::Ticket>>>
    openCallsInProject(aid::ProjectId project) override;

    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<std::vector<aid::MembershipDelta>>>
    refreshMembership() override;

    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<void>> close(aid::TicketId id) override;

    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<std::optional<aid::WebhookDecode>>>
    decodeWebhook(std::string payload) override;

    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<void>> ping() override;

private:
    // Order matters: helpers reference each other by reference, and
    // construction-order is field-declaration-order.
    std::unique_ptr<aid::infrastructure::HttpClient> httpClient_;
    aid::crosscutting::TicketSystemConfig opCfg_;
    aid::crosscutting::UiConfig uiCfg_;
    CustomFieldMap fields_;
    // Retained (a copy also lives inside OpHttp) so decodeWebhook can run its
    // self-echo grace delay on the same domain loop.
    Sleeper sleeper_;
    RealHttpDispatcher dispatcher_;
    OpStatusMap statusMap_;
    // Declared before tickets_: OpTicketRepo records produced versions + the
    // last-known callHandler set here, and decodeWebhook reads both back.
    ProducedLedger producedLedger_;
    HandlerLedger handlerLedger_;
    OpHttp http_;
    OpUserRepo users_;
    OpTicketRepo tickets_;
    OpDashboardBuilder dashboard_;
};

} // namespace aid::adapters::openproject
