#include "aid/adapters/openproject/OpenProjectAdapter.h"

#include <trantor/net/EventLoop.h>

#include <chrono>
#include <coroutine>
#include <cstring>
#include <exception>
#include <memory>
#include <new>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <utility>

#include "aid/abi/PluginAbiTag.h"
#include "aid/abi/PluginContract.h"
#include "aid/adapters/openproject/internal/CustomFieldMap.h"
#include "aid/adapters/openproject/internal/HttpDispatcher.h"
#include "aid/adapters/openproject/internal/payload.h"
#include "aid/adapters/support/HttpSupport.h"
#include "aid/crosscutting/Logger.h"
#include "aid/plumbing/Error.h"

// Plugin entry visibility: the rest of the .so is built with
// -fvisibility=hidden (PROPERTIES CXX_VISIBILITY_PRESET hidden), so the
// three extern "C" symbols below are the entire public surface and the
// only thing `nm -D` should turn up after the build.

#define AID_PLUGIN_EXPORT __attribute__((visibility("default")))

namespace aid::adapters::openproject {

// ─── Façade construction + forwards ────────────────────────────────────

OpenProjectAdapter::OpenProjectAdapter(std::unique_ptr<aid::infrastructure::HttpClient> http,
                                       aid::crosscutting::TicketSystemConfig opCfg,
                                       aid::crosscutting::UiConfig uiCfg, CustomFieldMap fields,
                                       Sleeper sleeper)
    : httpClient_(std::move(http)), opCfg_(std::move(opCfg)), uiCfg_(std::move(uiCfg)),
      fields_(std::move(fields)), sleeper_(std::move(sleeper)), dispatcher_(*httpClient_),
      statusMap_(OpStatusMap::fromConfig(opCfg_)),
      http_(dispatcher_, opCfg_.baseUrl, opCfg_.apiToken, sleeper_), users_(http_),
      tickets_(http_, users_, statusMap_, opCfg_, fields_, &producedLedger_, &handlerLedger_),
      dashboard_(users_, tickets_, opCfg_, uiCfg_) {
}

void OpenProjectAdapter::cancelPendingRequests() noexcept {
    // Abort any in-flight OpenProject request so a worker
    // suspended inside one of our helpers (tickets_/users_/http_) resumes at
    // once with a terminal error. main() calls this during the graceful drain,
    // while this adapter is fully alive, then waits for the mailboxes to go
    // quiescent before releasing the plugin — so the resumed chains unwind
    // through live members. httpClient_ is non-null for the adapter's lifetime
    // (set in the ctor init list).
    httpClient_->cancelInFlight();
}

aid::plumbing::Task<aid::plumbing::Result<aid::Ticket>>
OpenProjectAdapter::fetchById(aid::TicketId id) {
    return tickets_.fetchById(std::move(id));
}

aid::plumbing::Task<aid::plumbing::Result<std::optional<aid::Ticket>>>
OpenProjectAdapter::findByExactCallid(aid::CallId id) {
    return tickets_.findByExactCallid(std::move(id));
}

aid::plumbing::Task<aid::plumbing::Result<std::optional<aid::Ticket>>>
OpenProjectAdapter::findByCallidContains(aid::CallId id) {
    return tickets_.findByCallidContains(std::move(id));
}

aid::plumbing::Task<aid::plumbing::Result<std::optional<aid::Ticket>>>
OpenProjectAdapter::findLatestOpenCallInProject(aid::ProjectId project) {
    return tickets_.findLatestOpenCallInProject(std::move(project));
}

aid::plumbing::Task<aid::plumbing::Result<std::optional<aid::Ticket>>>
OpenProjectAdapter::findOpenInProjectBySubject(aid::ProjectId project, std::string_view subject) {
    return tickets_.findOpenInProjectBySubject(std::move(project), subject);
}

aid::plumbing::Task<aid::plumbing::Result<std::optional<aid::Ticket>>>
OpenProjectAdapter::findOpenInProjectByCallerNumber(aid::ProjectId project,
                                                    aid::PhoneNumber caller) {
    return tickets_.findOpenInProjectByCallerNumber(std::move(project), std::move(caller));
}

aid::plumbing::Task<aid::plumbing::Result<std::optional<aid::UserHandle>>>
OpenProjectAdapter::resolveUser(std::string_view login) {
    return users_.resolveLogin(login);
}

aid::plumbing::Task<aid::plumbing::Result<std::vector<aid::ProjectId>>>
OpenProjectAdapter::listProjectsForUser(aid::UserHandle user) {
    return users_.projectsForUser(std::move(user));
}

aid::plumbing::Task<aid::plumbing::Result<std::vector<aid::DashboardEntry>>>
OpenProjectAdapter::listDashboard(aid::UserHandle viewer) {
    return dashboard_.build(std::move(viewer));
}

aid::DashboardEntry OpenProjectAdapter::buildEntry(const aid::Ticket& ticket,
                                                   aid::UserHandle viewer) {
    return dashboard_.buildEntry(ticket, std::move(viewer));
}

aid::plumbing::Task<aid::plumbing::Result<aid::TicketId>>
OpenProjectAdapter::create(const aid::NewTicket& ticket) {
    return tickets_.create(ticket);
}

aid::plumbing::Task<aid::plumbing::Result<aid::Ticket>>
OpenProjectAdapter::save(aid::TicketId id, aid::ports::TicketReducer reduce) {
    // OpTicketRepo::save fetches the ticket, applies the reducer to the fresh
    // state, and re-applies it on every 409 retry. Pass through.
    return tickets_.save(std::move(id), std::move(reduce));
}

aid::plumbing::Task<aid::plumbing::Result<void>>
OpenProjectAdapter::addCallHandler(aid::TicketId id, aid::UserHandle login) {
    return tickets_.addCallHandler(std::move(id), std::move(login));
}

aid::plumbing::Task<aid::plumbing::Result<std::vector<aid::UserHandle>>>
OpenProjectAdapter::recipientsFor(const aid::Ticket& ticket) {
    return tickets_.recipientsFor(ticket);
}

aid::plumbing::Task<aid::plumbing::Result<std::vector<aid::Ticket>>>
OpenProjectAdapter::openCallsInProject(aid::ProjectId project) {
    return tickets_.openCallsInProject(std::move(project));
}

aid::plumbing::Task<aid::plumbing::Result<std::vector<aid::MembershipDelta>>>
OpenProjectAdapter::refreshMembership() {
    return users_.refreshMembership();
}

aid::plumbing::Task<aid::plumbing::Result<void>> OpenProjectAdapter::close(aid::TicketId id) {
    return tickets_.closeTwoStep(std::move(id));
}

aid::plumbing::Task<aid::plumbing::Result<std::optional<aid::WebhookDecode>>>
OpenProjectAdapter::decodeWebhook(std::string payload) {
    using aid::plumbing::Error;
    using aid::plumbing::ErrorCode;

    // The OpenProject webhook envelope is {"action":"work_package:...",
    // "work_package":{<full HAL representation>}}. Parse leniently: accept a
    // bare HAL work_package too, so a backend that posts the resource directly
    // still decodes.
    nlohmann::json env;
    try {
        env = nlohmann::json::parse(payload);
    } catch (const std::exception& e) {
        co_return aid::plumbing::unexpected{
            Error{ErrorCode::InvalidInput,
                  std::string{"decodeWebhook: body is not valid JSON: "} + e.what(), std::nullopt}};
    }

    const nlohmann::json* wp = nullptr;
    if (auto it = env.find("work_package"); it != env.end() && it->is_object()) {
        wp = &*it;
    } else if (env.is_object() && env.contains("_links")) {
        wp = &env;
    }
    if (wp == nullptr) {
        co_return aid::plumbing::unexpected{
            Error{ErrorCode::InvalidInput, "decodeWebhook: payload has no work_package object",
                  std::nullopt}};
    }

    auto parsed = parseFromHal(*wp, fields_, statusMap_);
    if (!parsed) {
        co_return aid::plumbing::unexpected{parsed.error()};
    }
    aid::Ticket ticket = std::move(*parsed);

    // Grace delay: a create()/save() this daemon just issued records its
    // produced version only once OpenProject answers the PATCH/POST. With
    // journal aggregation set to 0 the echo webhook can race that response, so
    // we wait briefly before consulting the ledger. This delay is NOT the match
    // criterion — suppression is still an EXACT (id, version) hit, so a human
    // edit landing within the window (necessarily at a higher version) passes.
    co_await sleeper_(ProducedLedger::kEchoGraceDelay);

    if (producedLedger_.contains(ticket.id, ticket.lockVersion)) {
        // Our own echo — already pushed as a live delta by the originating use
        // case, and the originating write already refreshed the handler ledger,
        // so there is nothing to diff. Drop it.
        co_return std::optional<aid::WebhookDecode>{};
    }

    // Genuine external edit. Compute which recipients lost visibility because an
    // admin removed them from the callHandler CSV (this also installs the new set
    // as the baseline for the next webhook). Best effort: a membership-lookup
    // failure must not lose the whole webhook — the ticket_upsert side still goes
    // out, and the stale row self-heals on the recipient's next dashboard reload.
    std::vector<aid::UserHandle> dropped;
    auto droppedRes = co_await tickets_.droppedRecipientsOnWebhook(ticket);
    if (droppedRes) {
        dropped = std::move(*droppedRes);
    } else {
        aid::crosscutting::Logger::instance().warn(
            "decodeWebhook: dropped-recipient lookup failed for ticket " + ticket.id.v +
            ", no ticket_remove sent: " + droppedRes.error().message);
    }

    co_return std::optional<aid::WebhookDecode>{
        aid::WebhookDecode{std::move(ticket), std::move(dropped)}};
}

aid::plumbing::Task<aid::plumbing::Result<void>> OpenProjectAdapter::ping() {
    // Cold-start ping: cheap auth'd GET, success = "reachable".
    // OpHttp::get already maps non-2xx into Error; we discard the body.
    auto resp = co_await http_.get("/api/v3/users/me");
    if (!resp) {
        co_return aid::plumbing::unexpected{resp.error()};
    }
    co_return aid::plumbing::Result<void>{};
}

namespace {

// Sleep awaiter that schedules its continuation on a trantor::EventLoop
// after the requested duration. Same pattern as HttpClient.cpp's local
// SleepAwaiter — duplicated here so the plugin doesn't poke at
// infrastructure internals.
struct LoopSleepAwaiter {
    trantor::EventLoop& loop;
    std::chrono::milliseconds dur;

    [[nodiscard]] bool await_ready() const noexcept { return dur.count() <= 0; }

    void await_suspend(std::coroutine_handle<> h) const noexcept {
        const double secs = static_cast<double>(dur.count()) / 1000.0;
        loop.runAfter(secs, [h]() noexcept { h.resume(); });
    }

    void await_resume() const noexcept {}
};

Sleeper makeLoopSleeper(trantor::EventLoop& loop) {
    return [&loop](std::chrono::milliseconds d) -> aid::plumbing::Task<void> {
        co_await LoopSleepAwaiter{loop, d};
        co_return;
    };
}

// Parse the slice of config.json the factory was handed. The factory
// must NEVER let an exception escape: catch every parse failure here,
// log it, and return nullopt so the daemon can refuse the plugin with a
// clean error rather than crashing.
struct ParsedFactoryConfig {
    aid::crosscutting::TicketSystemConfig op;
    aid::crosscutting::UiConfig ui;
    CustomFieldMap fields;
};

std::optional<ParsedFactoryConfig> parseFactoryConfig(const std::string& configJson) {
    using aid::crosscutting::Logger;
    using aid::crosscutting::LogType;
    try {
        const auto j = nlohmann::json::parse(configJson);
        if (!j.is_object()) {
            Logger::instance().error("openproject plugin: config_json must be an object",
                                     LogType::BACKEND);
            return std::nullopt;
        }

        ParsedFactoryConfig out;

        auto requireString = [&](const nlohmann::json& root,
                                 const char* k) -> std::optional<std::string> {
            auto it = root.find(k);
            if (it == root.end() || !it->is_string()) {
                Logger::instance().error(
                    std::string{"openproject plugin: config_json missing string field "} + k,
                    LogType::BACKEND);
                return std::nullopt;
            }
            return it->get<std::string>();
        };

        if (auto v = requireString(j, "baseUrl"); v)
            out.op.baseUrl = *v;
        else
            return std::nullopt;
        if (auto v = requireString(j, "apiToken"); v)
            out.op.apiToken = *v;
        else
            return std::nullopt;
        if (auto v = requireString(j, "statusNew"); v)
            out.op.statusNew.v = *v;
        else
            return std::nullopt;
        if (auto v = requireString(j, "statusInProgress"); v)
            out.op.statusInProgress.v = *v;
        else
            return std::nullopt;
        if (auto v = requireString(j, "statusClosed"); v)
            out.op.statusClosed.v = *v;
        else
            return std::nullopt;
        if (auto v = requireString(j, "typeCall"); v)
            out.op.typeCall = *v;
        else
            return std::nullopt;

        // projectNames is optional — operator may not have surfaced any yet.
        if (auto pn = j.find("projectNames"); pn != j.end()) {
            if (!pn->is_object()) {
                Logger::instance().error("openproject plugin: projectNames must be an object",
                                         LogType::BACKEND);
                return std::nullopt;
            }
            for (auto it = pn->begin(); it != pn->end(); ++it) {
                if (!it.value().is_string()) {
                    Logger::instance().error(
                        "openproject plugin: projectNames values must be strings",
                        LogType::BACKEND);
                    return std::nullopt;
                }
                out.op.projectNames.emplace(aid::ProjectId{it.key()},
                                            it.value().get<std::string>());
            }
        }

        // Ui.projectWebBaseUrl — embedded in the same slice so a single
        // factory call carries everything the adapter needs.
        if (auto v = requireString(j, "projectWebBaseUrl"); v)
            out.ui.projectWebBaseUrl = *v;
        else
            return std::nullopt;

        // CustomFieldMap — numeric custom-field ids. Operator fills these
        // in by hand for now; once schema resolution lands in
        // Main, the daemon will compute the map and inject it here.
        auto fieldIds = j.find("customFieldIds");
        if (fieldIds == j.end() || !fieldIds->is_object()) {
            Logger::instance().error(
                "openproject plugin: customFieldIds object is required",
                LogType::BACKEND);
            return std::nullopt;
        }
        auto fieldId = [&](const char* k) -> std::optional<aid::CustomFieldId> {
            auto it = fieldIds->find(k);
            if (it == fieldIds->end() || !it->is_string()) {
                Logger::instance().error(std::string{"openproject plugin: customFieldIds."} + k +
                                             " missing or not a string",
                                         LogType::BACKEND);
                return std::nullopt;
            }
            return aid::CustomFieldId{it->get<std::string>()};
        };
        if (auto v = fieldId("callId"); v)
            out.fields.callId = *v;
        else
            return std::nullopt;
        if (auto v = fieldId("callerNumber"); v)
            out.fields.callerNumber = *v;
        else
            return std::nullopt;
        if (auto v = fieldId("calledNumber"); v)
            out.fields.calledNumber = *v;
        else
            return std::nullopt;
        if (auto v = fieldId("callStart"); v)
            out.fields.callStart = *v;
        else
            return std::nullopt;
        if (auto v = fieldId("callEnd"); v)
            out.fields.callEnd = *v;
        else
            return std::nullopt;
        if (auto v = fieldId("callLength"); v)
            out.fields.callLength = *v;
        else
            return std::nullopt;
        if (auto v = fieldId("callHandler"); v)
            out.fields.callHandler = *v;
        else
            return std::nullopt;

        return out;
    } catch (const std::exception& e) {
        Logger::instance().error(std::string{"openproject plugin: config_json parse threw: "} +
                                     e.what(),
                                 LogType::BACKEND);
        return std::nullopt;
    } catch (...) {
        Logger::instance().error("openproject plugin: config_json parse threw unknown",
                                 LogType::BACKEND);
        return std::nullopt;
    }
}

// Extract scheme+host+port for HttpClient from the configured baseUrl.
// drogon::HttpClient::newHttpClient wants "http://host:port" with no
// path; trim everything from the first '/' after the scheme on.
std::string httpClientBaseUrl(std::string_view fullBase) {
    // Shared scheme+host extraction (aid::adapters::support). OpenProject's
    // no-scheme policy: use the string as-is (drogon rejects it downstream).
    return aid::adapters::support::schemeAndHost(fullBase).value_or(std::string{fullBase});
}

} // namespace

} // namespace aid::adapters::openproject

// ─── extern "C" factory triplet ────────────────────────────────────────

extern "C" AID_PLUGIN_EXPORT aid::ports::TicketStore* create_TicketStore(const char* config_json,
                                                                         void* event_loop) {
    using aid::adapters::openproject::httpClientBaseUrl;
    using aid::adapters::openproject::makeLoopSleeper;
    using aid::adapters::openproject::OpenProjectAdapter;
    using aid::adapters::openproject::parseFactoryConfig;
    using aid::crosscutting::Logger;
    using aid::crosscutting::LogType;

    if (config_json == nullptr || event_loop == nullptr) {
        Logger::instance().error("openproject plugin: factory got nullptr config_json/event_loop",
                                 LogType::BACKEND);
        return nullptr;
    }

    try {
        auto parsed = parseFactoryConfig(std::string{config_json});
        if (!parsed) {
            // parseFactoryConfig has already logged the specific reason.
            return nullptr;
        }

        auto* loop = static_cast<trantor::EventLoop*>(event_loop);

        aid::infrastructure::UpstreamConfig httpCfg{};
        // AID may run on a different box than OpenProject, so allow a longer
        // overall request deadline as cheap remote-insurance (default is 30 s).
        // NOTE: connectTimeout is intentionally left at its default — HttpClient
        // does not apply it (only readTimeout, the single drogon request
        // deadline, and networkRetries are honoured), so bumping it is a no-op.
        httpCfg.readTimeout = std::chrono::seconds{60};
        auto http = std::make_unique<aid::infrastructure::HttpClient>(
            httpClientBaseUrl(parsed->op.baseUrl), httpCfg, *loop);

        return new OpenProjectAdapter(std::move(http), std::move(parsed->op), std::move(parsed->ui),
                                      std::move(parsed->fields), makeLoopSleeper(*loop));
    } catch (const std::bad_alloc&) {
        // OOM at construction. No exception crosses the
        // boundary; convert to nullptr return.
        Logger::instance().error("openproject plugin: factory OOM", LogType::BACKEND);
        return nullptr;
    } catch (const std::exception& e) {
        Logger::instance().error(std::string{"openproject plugin: factory threw: "} + e.what(),
                                 LogType::BACKEND);
        return nullptr;
    } catch (...) {
        Logger::instance().error("openproject plugin: factory threw unknown", LogType::BACKEND);
        return nullptr;
    }
}

extern "C" AID_PLUGIN_EXPORT void destroy_TicketStore(aid::ports::TicketStore* p) {
    // The daemon owns the deletion; we just `delete`. This runs from
    // Main's tail after all in-flight coroutines
    // are drained, so no work is racing the destructor.
    delete p;
}

extern "C" AID_PLUGIN_EXPORT int aid_plugin_api_version(void) {
    return 1;
}

// ABI layout guard: the daemon compares this against its own
// aid::abi::kPluginAbiLayoutTag and refuses to load on mismatch, turning a
// silent heap-corruption SIGSEGV into a clean startup refusal.
extern "C" AID_PLUGIN_EXPORT std::uint64_t aid_plugin_abi_layout_tag(void) {
    return aid::abi::kPluginAbiLayoutTag;
}

// BF3 stale-plugin guard: the behaviour-contract tag this `.so` was built
// against. The daemon logs + compares it at startup, and scripts/deploy.sh
// greps it from the just-copied `.so`, so a stale plugin that lags the daemon
// fails loudly instead of silently emitting no callHandler-drop deltas.
extern "C" AID_PLUGIN_EXPORT const char* aid_plugin_contract_tag(void) {
    return aid::abi::kPluginContractTag;
}
