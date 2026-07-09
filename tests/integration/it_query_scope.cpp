// Query-scope guard: proves at the HTTP wire level that the daemon fetches a
// FULL ticket set only on a dashboard read (login/refresh), and stays strictly
// single-ticket-scoped on a live delta (a dashboard UI edit or an inbound
// OpenProject webhook). It loads the REAL aid_openproject_plugin.so via
// PluginLoader (same path as src/main.cpp) and drives it against a recording
// MockHttpServer, then asserts on the exact (method, target) set the plugin
// issued. A regression that makes a delta trigger a full rebuild fails T2/T3.
//
// Companion to it_plugin_end_to_end.cpp — same harness, narrower question:
// "which OpenProject queries fire, and do we ever ask for more than we need?"

#include <gtest/gtest.h>
#include <trantor/net/EventLoop.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "FakeUiNotifier.h"
#include "IntegrationHarness.h"
#include "MockHttpServer.h"
#include "aid/infrastructure/PluginLoader.h"
#include "aid/plumbing/Result.h"
#include "aid/plumbing/Task.h"
#include "aid/ports/TicketStore.h"
#include "aid/ports/UiNotifier.h"
#include "aid/usecases/AppendComment.h"
#include "aid/usecases/CloseTicket.h"
#include "aid/usecases/TicketDeltaEmitter.h"
#include "aid/value-types/Dashboard.h"
#include "aid/value-types/Ids.h"
#include "aid/value-types/Ticket.h"

#ifndef AID_OPENPROJECT_PLUGIN_PATH
#error "AID_OPENPROJECT_PLUGIN_PATH must be defined by CMake"
#endif

namespace {

using aid::TicketId;
using aid::UserHandle;
using aid::fakes::FakeUiNotifier;
using aid::infrastructure::PluginLoader;
using aid::plumbing::ActionResult;
using aid::plumbing::Result;
using aid::plumbing::Task;
using aid::tests::integration::LoggerOnce;
using aid::tests::integration::LoopThread;
using aid::tests::integration::MockHttpServer;
using aid::tests::integration::MockRequest;
using aid::tests::integration::MockResponse;
using aid::usecases::AppendComment;
using aid::usecases::CloseTicket;
using aid::usecases::TicketDeltaEmitter;

constexpr const char* kTicketId = "100";

std::string opConfig(std::uint16_t port) {
    return std::string{R"({
        "baseUrl": "http://127.0.0.1:)"} +
           std::to_string(port) + std::string{R"(",
        "apiToken": "scope-token",
        "statusNew": "1", "statusInProgress": "2", "statusClosed": "3",
        "typeCall": "7",
        "projectNames": { "11": "Acme" },
        "projectWebBaseUrl": "http://127.0.0.1/projects",
        "customFieldIds": {
            "callId": "1", "callerNumber": "2", "calledNumber": "3",
            "callStart": "4", "callEnd": "5", "callLength": "6",
            "callHandler": "7"
        }
    })"};
}

// Minimal HAL work_package the adapter can parse: id, subject, lockVersion, the
// custom fields the use cases read, and the three required _links. Project 11
// has one member (user 9, "alice") so recipientsFor resolves a recipient.
std::string halTicket(int lockVersion, const std::string& statusHref) {
    nlohmann::json j;
    j["id"] = 100;
    j["subject"] = "Alice Example";
    j["description"]["raw"] = "";
    j["lockVersion"] = lockVersion;
    j["updatedAt"] = "2024-01-15T10:30:00Z";
    j["customField1"] = "scope-callid";
    j["customField2"] = "+491701234567";
    j["customField6"]["format"] = "markdown";
    j["customField6"]["raw"] = "";
    j["_links"]["project"]["href"] = "/api/v3/projects/11";
    j["_links"]["status"]["href"] = statusHref;
    j["_links"]["self"]["href"] = "/api/v3/work_packages/100";
    return j.dump();
}

std::string halCollectionOf(const std::string& ticketBody) {
    return std::string{R"({"_embedded":{"elements":[)"} + ticketBody + R"(]}})";
}

std::string emptyCollection() {
    return R"({"_embedded":{"elements":[]}})";
}

// _embedded collection for the viewer-login lookup (resolveLogin / hrefFor).
std::string usersCollection() {
    return R"({"_embedded":{"elements":[{"login":"alice","id":9,)"
           R"("_links":{"self":{"href":"/api/v3/users/9"}}}]}})";
}

// Single user object for the by-href resolution (loginForUserHref → GET /users/9).
std::string userObject() {
    return R"({"login":"alice","id":9,"_links":{"self":{"href":"/api/v3/users/9"}}})";
}

std::string projectsCollection() {
    return R"({"_embedded":{"elements":[{"id":11,)"
           R"("_links":{"self":{"href":"/api/v3/projects/11"}}}]}})";
}

// One membership whose principal is user 9 (alice).
std::string membershipsCollection() {
    return R"({"_embedded":{"elements":[{"_links":{"principal":{"href":"/api/v3/users/9"}}}]}})";
}

MockResponse json200(std::string body) {
    return MockResponse{200, "OK", "application/json", std::move(body)};
}

// One responder that answers every query shape the three scenarios can emit.
// It NEVER refuses a query — so if a delta path wrongly issued a list/projects
// query it would still succeed; the tests then catch it via count(...) == 0.
MockResponse opResponder(const MockRequest& r) {
    if (r.method == "GET" && r.target.find("/api/v3/users") != std::string::npos) {
        // ?filters=...login... → collection; /users/<id> → single object.
        if (r.target.find("filters=") != std::string::npos)
            return json200(usersCollection());
        return json200(userObject());
    }
    if (r.method == "GET" && r.target.find("/api/v3/projects") != std::string::npos)
        return json200(projectsCollection());
    if (r.method == "GET" && r.target.find("/api/v3/memberships") != std::string::npos)
        return json200(membershipsCollection());
    if (r.method == "GET" && r.target.find("/work_packages") != std::string::npos) {
        if (r.target.find("customField7") != std::string::npos)
            return json200(emptyCollection()); // dashboard handler arm
        if (r.target.find("filters=") != std::string::npos)
            return json200(halCollectionOf(halTicket(1, "/api/v3/statuses/1"))); // member arm
        return json200(halTicket(1, "/api/v3/statuses/1"));                      // fetch by id
    }
    if (r.method == "PATCH" && r.target.find("/work_packages/") != std::string::npos)
        return json200(halTicket(2, "/api/v3/statuses/1"));
    if (r.method == "POST" && r.target.find("/work_packages") != std::string::npos)
        return MockResponse{201, "Created", "application/json", R"({"id":100,"lockVersion":0})"};
    return json200(emptyCollection());
}

// ─── (method, target) predicates the assertions count over ─────────────────

bool isUsersLoginLookup(const MockRequest& r) {
    return r.method == "GET" && r.target.find("/api/v3/users") != std::string::npos &&
           r.target.find("filters=") != std::string::npos;
}
bool isProjectsForUser(const MockRequest& r) {
    return r.method == "GET" && r.target.find("/api/v3/projects") != std::string::npos;
}
bool isMemberships(const MockRequest& r) {
    return r.method == "GET" && r.target.find("/api/v3/memberships") != std::string::npos;
}
// Dashboard member arm: a filtered work_packages list that is NOT the handler arm.
bool isWpMemberArm(const MockRequest& r) {
    return r.method == "GET" && r.target.find("/work_packages") != std::string::npos &&
           r.target.find("filters=") != std::string::npos &&
           r.target.find("customField7") == std::string::npos;
}
bool isWpHandlerArm(const MockRequest& r) {
    return r.method == "GET" && r.target.find("/work_packages") != std::string::npos &&
           r.target.find("customField7") != std::string::npos;
}
// Any filtered work_packages LIST query (the full-fetch fingerprint).
bool isWpListQuery(const MockRequest& r) {
    return r.method == "GET" && r.target.find("/work_packages") != std::string::npos &&
           r.target.find("filters=") != std::string::npos;
}
// Single-ticket fetch: /work_packages/<id>, no filters.
bool isWpById(const MockRequest& r) {
    return r.method == "GET" && r.target.find("/work_packages/") != std::string::npos;
}
bool isWpPatch(const MockRequest& r) {
    return r.method == "PATCH" && r.target.find("/work_packages/") != std::string::npos;
}
bool isAnyWorkPackages(const MockRequest& r) {
    return r.target.find("/work_packages") != std::string::npos;
}

// Drive a Task<Result<T>>-returning factory on the domain loop (HttpClient is
// pinned there) and block for the result. Same shape as it_plugin_end_to_end.cpp.
template <class T>
Result<T> runOnLoop(trantor::EventLoop& loop, std::function<Task<Result<T>>()> factory) {
    struct Inflight {
        std::mutex m;
        std::unordered_map<int, std::unique_ptr<Task<void>>> tasks;
    };
    auto inflight = std::make_shared<Inflight>();
    auto prom = std::make_shared<std::promise<Result<T>>>();
    auto fut = prom->get_future();

    loop.queueInLoop([&loop, factory = std::move(factory), prom, inflight]() mutable {
        auto coro = [](std::function<Task<Result<T>>()> f,
                       std::shared_ptr<std::promise<Result<T>>> p, std::shared_ptr<Inflight> inf,
                       trantor::EventLoop* lp) -> Task<void> {
            try {
                auto r = co_await f();
                p->set_value(std::move(r));
            } catch (...) {
                p->set_exception(std::current_exception());
            }
            lp->queueInLoop([inf] {
                std::lock_guard lk{inf->m};
                inf->tasks.clear();
            });
        }(std::move(factory), prom, inflight, &loop);
        std::lock_guard lk{inflight->m};
        inflight->tasks.emplace(0, std::make_unique<Task<void>>(std::move(coro)));
    });
    return fut.get();
}

class QueryScope : public ::testing::Test {
protected:
    static std::unique_ptr<PluginLoader<aid::ports::TicketStore>>
    loadOpenProject(std::uint16_t port, trantor::EventLoop& loop) {
        auto loader = std::make_unique<PluginLoader<aid::ports::TicketStore>>();
        const auto r =
            loader->loadWithLoop(AID_OPENPROJECT_PLUGIN_PATH, "create_TicketStore",
                                 "destroy_TicketStore", opConfig(port), &loop, ::geteuid());
        EXPECT_TRUE(r.has_value()) << (r.has_value() ? std::string{} : r.error().message);
        return loader;
    }

    LoggerOnce loggerInit_{};
};

} // namespace

// T1 — a dashboard read fires the FULL query set: viewer-login lookup,
// projects-for-user, the member-projects work_packages arm, and the
// cross-project handler work_packages arm.
TEST_F(QueryScope, DashboardReadFiresAllQueries) {
    MockHttpServer opSrv(opResponder);
    LoopThread lt;
    auto opPlugin = loadOpenProject(opSrv.port(), lt.loop());
    ASSERT_NE(opPlugin->get(), nullptr);
    auto& store = *opPlugin->get();

    auto result = runOnLoop<std::vector<aid::DashboardEntry>>(
        lt.loop(), [&] { return store.listDashboard(UserHandle{"alice"}); });

    ASSERT_TRUE(result.has_value())
        << (result.has_value() ? std::string{} : result.error().message);

    EXPECT_GE(opSrv.count(isUsersLoginLookup), 1U) << "must resolve the viewer's login";
    EXPECT_GE(opSrv.count(isProjectsForUser), 1U) << "must list the viewer's projects";
    EXPECT_GE(opSrv.count(isWpMemberArm), 1U) << "must query open call tickets in member projects";
    EXPECT_GE(opSrv.count(isWpHandlerArm), 1U) << "must query the cross-project handler arm";
}

// T2 — a comment added through the daemon dashboard UI is single-ticket scoped:
// exactly the one ticket is fetched + patched, recipients resolved; NO full-fetch
// list query and NO projects-for-user query fire.
TEST_F(QueryScope, UiCommentIsSingleTicketScoped) {
    MockHttpServer opSrv(opResponder);
    LoopThread lt;
    auto opPlugin = loadOpenProject(opSrv.port(), lt.loop());
    ASSERT_NE(opPlugin->get(), nullptr);
    auto& store = *opPlugin->get();

    FakeUiNotifier ui;
    AppendComment uc{store, ui};
    auto result = runOnLoop<ActionResult>(lt.loop(), [&] {
        return uc.run(TicketId{kTicketId}, "a live comment", UserHandle{"alice"});
    });

    ASSERT_TRUE(result.has_value())
        << (result.has_value() ? std::string{} : result.error().message);
    EXPECT_TRUE(result->ok);

    EXPECT_GE(opSrv.count(isWpById), 1U) << "the one ticket is fetched by id";
    EXPECT_GE(opSrv.count(isWpPatch), 1U) << "the one ticket is patched";
    EXPECT_EQ(opSrv.count(isWpListQuery), 0U) << "a delta must NOT run a full work_packages list";
    EXPECT_EQ(opSrv.count(isProjectsForUser), 0U)
        << "a delta must NOT re-list the viewer's projects";
    // Recipient-scoped push: alice is the project's sole member.
    EXPECT_EQ(ui.ticketUpserts.size(), 1U) << "delta pushed to exactly the one recipient";
}

// T2b — closing a ticket through the UI is likewise single-ticket scoped.
TEST_F(QueryScope, UiCloseIsSingleTicketScoped) {
    MockHttpServer opSrv(opResponder);
    LoopThread lt;
    auto opPlugin = loadOpenProject(opSrv.port(), lt.loop());
    ASSERT_NE(opPlugin->get(), nullptr);
    auto& store = *opPlugin->get();

    FakeUiNotifier ui;
    CloseTicket uc{store, ui};
    auto result = runOnLoop<ActionResult>(
        lt.loop(), [&] { return uc.run(TicketId{kTicketId}, UserHandle{"alice"}); });

    ASSERT_TRUE(result.has_value())
        << (result.has_value() ? std::string{} : result.error().message);
    EXPECT_TRUE(result->ok);

    EXPECT_GE(opSrv.count(isWpPatch), 1U) << "the status-flow PATCH(es) target the one ticket";
    EXPECT_EQ(opSrv.count(isWpListQuery), 0U) << "a delta must NOT run a full work_packages list";
    EXPECT_EQ(opSrv.count(isProjectsForUser), 0U)
        << "a delta must NOT re-list the viewer's projects";
}

// T3 — an inbound OpenProject webhook (external edit) emits a delta WITHOUT
// fetching the ticket at all: the ticket data rides in the payload, so the only
// OpenProject traffic is recipient resolution. NO work_packages query of ANY
// kind, NO projects query.
TEST_F(QueryScope, WebhookEmitsDeltaWithoutFetchingTicket) {
    MockHttpServer opSrv(opResponder);
    LoopThread lt;
    auto opPlugin = loadOpenProject(opSrv.port(), lt.loop());
    ASSERT_NE(opPlugin->get(), nullptr);
    auto& store = *opPlugin->get();

    FakeUiNotifier ui;
    const std::string payload = std::string{R"({"action":"work_package:updated","work_package":)"} +
                                halTicket(1, "/api/v3/statuses/1") + "}";

    auto result = runOnLoop<int>(lt.loop(), [&]() -> Task<Result<int>> {
        auto decoded = co_await store.decodeWebhook(payload);
        if (!decoded || !decoded->has_value())
            co_return 0; // parse error or self-echo (suppressed)
        TicketDeltaEmitter emitter{store, ui};
        (void)co_await emitter.emitTicketDelta(std::move((*decoded)->ticket));
        co_return 1;
    });

    ASSERT_TRUE(result.has_value())
        << (result.has_value() ? std::string{} : result.error().message);
    EXPECT_EQ(*result, 1) << "an external edit must decode and emit a delta";

    EXPECT_EQ(opSrv.count(isAnyWorkPackages), 0U)
        << "the webhook carries the ticket — the daemon must not fetch it";
    EXPECT_EQ(opSrv.count(isProjectsForUser), 0U) << "a webhook delta must NOT list projects";
    EXPECT_GE(opSrv.count(isMemberships), 1U) << "only recipient resolution should hit OpenProject";
    EXPECT_EQ(ui.ticketUpserts.size(), 1U) << "delta pushed to exactly the one recipient";
}
