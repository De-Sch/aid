// The highest-value coverage gap — an end-to-end test that drives the
// REAL OpenProject + DaviCal plugin .so files (dlopen'd via PluginLoader, the
// same path src/main.cpp uses) through the full daemon chain
// (CallController → Mailbox → use case → plugin port → HttpClient) against a
// local mock HTTP server. Every other integration test fakes the ports; this
// one exercises the plugins' own wire code and the extern "C" boundary, fully
// reproducibly in CI (no docker, no live OpenProject/DaviCal).
//
// Two focused scenarios:
//   1. Call lifecycle: POST incoming → accepted → hangup for one callid; assert
//      the OpenProject mock saw create + the status-flow PATCHes + the user
//      resolution, and the DaviCal mock saw the contact REPORT.
//   2. Dashboard read with a resolved contact: GetDashboard against the real
//      plugins; the active-call ticket's caller number resolves to a DaviCal
//      contact (addressCallInformation).
//
// Canned upstream payloads mirror the wire shapes encoded in the adapter unit
// tests (tests/adapters/openproject_plugin/*, tests/adapters/davical_plugin/*).

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/HttpTypes.h>
#include <gtest/gtest.h>
#include <trantor/net/EventLoop.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

#include "FakeClock.h"
#include "FakeUiNotifier.h"
#include "IntegrationHarness.h"
#include "MockHttpServer.h"
#include "aid/controllers/CallController.h"
#include "aid/crosscutting/Config.h"
#include "aid/crosscutting/CorrelationId.h"
#include "aid/crosscutting/Logger.h"
#include "aid/domain/CallLineFormatter.h"
#include "aid/infrastructure/Mailbox.h"
#include "aid/infrastructure/PluginLoader.h"
#include "aid/infrastructure/Wal.h"
#include "aid/plumbing/Result.h"
#include "aid/plumbing/Task.h"
#include "aid/ports/AddressBook.h"
#include "aid/ports/TicketStore.h"
#include "aid/usecases/GetDashboard.h"
#include "aid/usecases/HandleAcceptedCall.h"
#include "aid/usecases/HandleHangup.h"
#include "aid/usecases/HandleIncomingCall.h"
#include "aid/value-types/Dashboard.h"
#include "aid/value-types/Ids.h"

#ifndef AID_OPENPROJECT_PLUGIN_PATH
#error "AID_OPENPROJECT_PLUGIN_PATH must be defined by CMake"
#endif
#ifndef AID_DAVICAL_PLUGIN_PATH
#error "AID_DAVICAL_PLUGIN_PATH must be defined by CMake"
#endif

namespace {

using aid::CallId;
using aid::UserHandle;
using aid::controllers::CallController;
using aid::crosscutting::Config;
using aid::crosscutting::CorrelationId;
using aid::crosscutting::Logger;
using aid::fakes::FakeClock;
using aid::fakes::FakeUiNotifier;
using aid::infrastructure::Mailbox;
using aid::infrastructure::PluginLoader;
using aid::infrastructure::Wal;
using aid::plumbing::Result;
using aid::plumbing::Task;
using aid::tests::integration::LoggerOnce;
using aid::tests::integration::LoopThread;
using aid::tests::integration::MockHttpServer;
using aid::tests::integration::MockRequest;
using aid::tests::integration::MockResponse;
using aid::tests::integration::waitUntil;
using aid::usecases::GetDashboard;
using aid::usecases::HandleAcceptedCall;
using aid::usecases::HandleHangup;
using aid::usecases::HandleIncomingCall;
using json = nlohmann::json;

constexpr const char* kCaller = "+491701234567";
constexpr const char* kCallid = "e2e-1";

std::string opConfig(std::uint16_t port) {
    return std::string{R"({
        "baseUrl": "http://127.0.0.1:)"} +
           std::to_string(port) + std::string{R"(",
        "apiToken": "e2e-token",
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

std::string davicalConfig(std::uint16_t port) {
    return std::string{R"({
        "libPath": "ignored-at-test-time",
        "bookAddresses": "http://127.0.0.1:)"} +
           std::to_string(port) + std::string{R"(/dav/addresses/",
        "bookCompanies": "http://127.0.0.1:)"} +
           std::to_string(port) + std::string{R"(/dav/companies/",
        "user": "aid", "password": "1234", "defaultRegion": "DE"
    })"};
}

// Minimal HAL work_package the adapter can parse: id, subject, lockVersion,
// the custom fields the use cases read, and the three required _links. The
// call-log lines live in callLength (customField6, a Formattable field); the
// description (comment section) is left empty in these fixtures.
json halTicket(int lockVersion, const std::string& statusHref, const std::string& callLog) {
    json j;
    j["id"] = 100;
    j["subject"] = "Alice Example";
    j["description"]["raw"] = "";
    j["lockVersion"] = lockVersion;
    j["updatedAt"] = "2024-01-15T10:30:00Z";
    j["customField1"] = kCallid;              // callId
    j["customField2"] = kCaller;              // callerNumber
    j["customField6"]["format"] = "markdown"; // callLength
    j["customField6"]["raw"] = callLog;       // appended call-log lines
    j["_links"]["project"]["href"] = "/api/v3/projects/11";
    j["_links"]["status"]["href"] = statusHref;
    j["_links"]["self"]["href"] = "/api/v3/work_packages/100";
    return j;
}

std::string halCollectionOf(const json& ticket) {
    json env;
    env["_embedded"]["elements"] = json::array({ticket});
    return env.dump();
}

std::string emptyCollection() {
    return R"({"_embedded":{"elements":[]}})";
}

std::string usersCollection() {
    return R"({"_embedded":{"elements":[{"login":"alice","id":9,)"
           R"("_links":{"self":{"href":"/api/v3/users/9"}}}]}})";
}

std::string projectsCollection() {
    return R"({"_embedded":{"elements":[{"id":11,)"
           R"("_links":{"self":{"href":"/api/v3/projects/11"}}}]}})";
}

// CardDAV multistatus carrying one vCard whose TEL matches kCaller and whose
// X-CUSTOM1 routes to project 11 (so the contact is "known"). Mirrors the
// wrap() shape in tests/adapters/davical_plugin/test_dc_vcard_parser.cpp.
std::string vcardMultistatus() {
    return R"(<?xml version="1.0" encoding="utf-8"?>
<d:multistatus xmlns:d="DAV:" xmlns:card="urn:ietf:params:xml:ns:carddav">
  <d:response>
    <d:href>/dav/addresses/alice.vcf</d:href>
    <d:propstat>
      <d:prop>
        <card:address-data>BEGIN:VCARD
VERSION:3.0
FN:Alice Example
ORG:Example GmbH
TEL:+491701234567
X-CUSTOM1:11
END:VCARD
</card:address-data>
      </d:prop>
      <d:status>HTTP/1.1 200 OK</d:status>
    </d:propstat>
  </d:response>
</d:multistatus>)";
}

MockResponse json200(std::string body) {
    return MockResponse{200, "OK", "application/json", std::move(body)};
}

MockResponse davicalReport(const MockRequest& r) {
    if (r.method == "REPORT") {
        return MockResponse{207, "Multi-Status", "application/xml", vcardMultistatus()};
    }
    // PROPFIND ping or anything else: an empty multistatus reads as reachable.
    return MockResponse{207, "Multi-Status", "application/xml",
                        R"(<d:multistatus xmlns:d="DAV:"/>)"};
}

bool isPatch(const MockRequest& r) {
    return r.method == "PATCH" && r.target.find("/work_packages/") != std::string::npos;
}

// Drive a Task<Result<T>>-returning factory on the domain loop (HttpClient is
// pinned there) and block for the result. Same shape as the per-fixture driver
// in tests/adapters/openproject_plugin/test_plugin_smoke.cpp.
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

class PluginEndToEnd : public ::testing::Test {
protected:
    void SetUp() override {
        const auto pid = static_cast<std::uint64_t>(::getpid());
        static std::atomic<std::uint64_t> counter{0};
        const auto n = counter.fetch_add(1, std::memory_order_relaxed);
        dir_ = std::filesystem::temp_directory_path() /
               ("aid_plugin_e2e_" + std::to_string(pid) + "_" + std::to_string(n));
        std::filesystem::create_directories(dir_);
        walPath_ = (dir_ / "inbox.log").string();
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    static std::unique_ptr<PluginLoader<aid::ports::TicketStore>>
    loadOpenProject(std::uint16_t port, trantor::EventLoop& loop) {
        auto loader = std::make_unique<PluginLoader<aid::ports::TicketStore>>();
        const auto r =
            loader->loadWithLoop(AID_OPENPROJECT_PLUGIN_PATH, "create_TicketStore",
                                 "destroy_TicketStore", opConfig(port), &loop, ::geteuid());
        EXPECT_TRUE(r.has_value()) << (r.has_value() ? std::string{} : r.error().message);
        return loader;
    }

    static std::unique_ptr<PluginLoader<aid::ports::AddressBook>>
    loadDaviCal(std::uint16_t port, trantor::EventLoop& loop) {
        auto loader = std::make_unique<PluginLoader<aid::ports::AddressBook>>();
        const auto r =
            loader->loadWithLoop(AID_DAVICAL_PLUGIN_PATH, "create_AddressBook",
                                 "destroy_AddressBook", davicalConfig(port), &loop, ::geteuid());
        EXPECT_TRUE(r.has_value()) << (r.has_value() ? std::string{} : r.error().message);
        return loader;
    }

    static std::string callJson(std::string_view event, bool withUser) {
        json j;
        j["event"] = event;
        j["callid"] = kCallid;
        j["remote"] = kCaller;
        if (event == "Incoming Call" || event == "Accepted Call") {
            j["dialed"] = "+4930";
        }
        if (withUser) {
            j["user"] = "alice";
        }
        return j.dump();
    }

    drogon::HttpStatusCode post(CallController& c, const std::string& body) {
        auto req = drogon::HttpRequest::newHttpRequest();
        req->setMethod(drogon::Post);
        req->setBody(body);
        std::optional<drogon::HttpStatusCode> status;
        c.handlePost(req, [&](const drogon::HttpResponsePtr& r) { status = r->getStatusCode(); });
        EXPECT_TRUE(status.has_value());
        return status.value_or(drogon::k500InternalServerError);
    }

    LoggerOnce loggerInit_{};
    FakeClock clock_{};
    CorrelationId cid_{};
    std::filesystem::path dir_;
    std::string walPath_;
};

} // namespace

// Scenario 1: incoming → accepted → hangup for one callid, driven through the
// real CallController + Mailbox + use cases + real plugin .so files.
TEST_F(PluginEndToEnd, CallLifecycleDrivesRealPluginsAgainstMockOpenProject) {
    // OpenProject mock: route by method + target. The incoming flow dedups by
    // caller number (GET without the callid) → empty → create; accepted/hangup
    // look up by callid (GET containing kCallid) → the created ticket.
    MockHttpServer opSrv([](const MockRequest& r) -> MockResponse {
        if (r.method == "POST" && r.target.find("/work_packages") != std::string::npos) {
            return MockResponse{201, "Created", "application/json", R"({"id":100})"};
        }
        if (isPatch(r)) {
            return json200(halTicket(2, "/api/v3/statuses/2", "").dump());
        }
        if (r.method == "GET" && r.target.find("/api/v3/users") != std::string::npos) {
            return json200(usersCollection());
        }
        if (r.method == "GET" && r.target.find("/work_packages") != std::string::npos) {
            // Direct fetch by id (no "filters=" query) — addCallHandler's refetch.
            // Return a single parseable work package (no customField7 → alice is
            // not yet a handler, so the accept records her).
            if (r.target.find("filters=") == std::string::npos) {
                return json200(halTicket(1, "/api/v3/statuses/1", "").dump());
            }
            if (r.target.find(kCallid) != std::string::npos) {
                return json200(halCollectionOf(halTicket(1, "/api/v3/statuses/1", "")));
            }
            return json200(emptyCollection());
        }
        return json200(emptyCollection());
    });
    MockHttpServer davSrv(davicalReport);

    LoopThread lt;
    auto opPlugin = loadOpenProject(opSrv.port(), lt.loop());
    auto davPlugin = loadDaviCal(davSrv.port(), lt.loop());
    ASSERT_NE(opPlugin->get(), nullptr);
    ASSERT_NE(davPlugin->get(), nullptr);

    FakeUiNotifier ui;
    const Config::TicketRouting routing{aid::ProjectId{"3"}, "Incognito Caller"};
    HandleIncomingCall incoming{*opPlugin->get(), *davPlugin->get(), ui, clock_, routing};
    HandleAcceptedCall accepted{*opPlugin->get(), ui, clock_};
    HandleHangup hangup{*opPlugin->get(), ui, clock_};

    Wal wal{walPath_, clock_};
    Mailbox::Handlers handlers;
    handlers.incoming = [&](const aid::IncomingCall& ev, bool replay) -> Task<Result<void>> {
        co_return co_await incoming.run(ev, replay);
    };
    handlers.accepted = [&](const aid::AcceptedCall& ev) -> Task<Result<void>> {
        co_return co_await accepted.run(ev);
    };
    handlers.hangup = [&](const aid::HangupCall& ev) -> Task<Result<void>> {
        co_return co_await hangup.run(ev);
    };
    auto noop = [](auto&&...) -> Task<Result<void>> { co_return Result<void>{}; };
    handlers.outgoing = noop;
    handlers.transfer = noop;
    Mailbox mb{lt.loop(), wal, Logger::instance(), std::move(handlers),
               &CallController::decodeJson};
    CallController controller{wal, mb, Logger::instance(), cid_};

    EXPECT_EQ(post(controller, callJson("Incoming Call", /*withUser=*/false)),
              drogon::k202Accepted);
    EXPECT_EQ(post(controller, callJson("Accepted Call", /*withUser=*/true)), drogon::k202Accepted);
    EXPECT_EQ(post(controller, callJson("Hangup", /*withUser=*/false)), drogon::k202Accepted);

    // Per-callid serialization guarantees the three events run in order. The
    // accepted flow now PATCHes twice (the status/assignee save + the dedicated
    // callHandler merge patch), and hangup's save is the last PATCH — three in
    // all. count() is mutex-protected.
    ASSERT_TRUE(waitUntil([&] { return opSrv.count(isPatch) >= 3; }))
        << "accepted (save + callHandler) + hangup must each PATCH through the real plugin";

    // Create: exactly one POST, to the contact's project (11).
    EXPECT_EQ(opSrv.count([](const MockRequest& r) {
        return r.method == "POST" &&
               r.target.find("/api/v3/projects/11/work_packages") != std::string::npos;
    }),
              1U);
    // User resolution happened during the accepted flow.
    EXPECT_GE(opSrv.count([](const MockRequest& r) {
        return r.method == "GET" && r.target.find("/api/v3/users") != std::string::npos;
    }),
              1U);
    EXPECT_EQ(opSrv.count(isPatch), 3U);
    // The DaviCal contact REPORT fired during the incoming lookup.
    EXPECT_GE(davSrv.count([](const MockRequest& r) { return r.method == "REPORT"; }), 1U);
}

// Scenario 2: /ui/dashboard read where the viewer has an active call whose
// caller number resolves to a DaviCal contact. Drives GetDashboard through the
// real OpenProject + DaviCal plugins.
TEST_F(PluginEndToEnd, DashboardReadResolvesContactThroughRealPlugins) {
    LoopThread lt;

    // The active-call ticket's description carries an open call line for the
    // viewer so the dashboard builder marks it active (and GetDashboard then
    // resolves the caller's contact). Build the line with the same formatter
    // the live accepted-flow uses.
    const aid::Timestamp callStart{std::chrono::milliseconds{1'700'000'000'000}};
    const std::string openLine =
        aid::domain::CallLineFormatter::buildStart(UserHandle{"alice"}, callStart, CallId{kCallid});

    MockHttpServer opSrv([openLine](const MockRequest& r) -> MockResponse {
        if (r.method == "GET" && r.target.find("/api/v3/users") != std::string::npos) {
            return json200(usersCollection());
        }
        if (r.method == "GET" && r.target.find("/api/v3/projects") != std::string::npos) {
            return json200(projectsCollection());
        }
        if (r.method == "GET" && r.target.find("/work_packages") != std::string::npos) {
            if (r.target.find("assignee") != std::string::npos) {
                return json200(emptyCollection()); // assigned-to-viewer query
            }
            // open call tickets in the viewer's projects
            return json200(halCollectionOf(halTicket(1, "/api/v3/statuses/1", openLine)));
        }
        return json200(emptyCollection());
    });
    MockHttpServer davSrv(davicalReport);

    auto opPlugin = loadOpenProject(opSrv.port(), lt.loop());
    auto davPlugin = loadDaviCal(davSrv.port(), lt.loop());
    ASSERT_NE(opPlugin->get(), nullptr);
    ASSERT_NE(davPlugin->get(), nullptr);

    GetDashboard dashboard{*opPlugin->get(), *davPlugin->get()};

    auto result = runOnLoop<aid::DashboardView>(lt.loop(),
                                                [&] { return dashboard.run(UserHandle{"alice"}); });

    ASSERT_TRUE(result.has_value())
        << (result.has_value() ? std::string{} : result.error().message);
    EXPECT_FALSE(result->tickets.empty()) << "dashboard must list the open call ticket";
    EXPECT_EQ(result->tickets.front().projectName, "Acme")
        << "the entry must carry the project's configured display name";
    // activeCallForViewer is computed from the open call line, which now lives
    // in the callLength field (not description). The comment-section description
    // is empty in this fixture.
    EXPECT_TRUE(result->tickets.front().activeCallForViewer.has_value())
        << "the open call line in callLength must mark the entry active";
    EXPECT_TRUE(result->tickets.front().description.empty())
        << "the comment-section description holds no call-log lines";
    ASSERT_TRUE(result->active.has_value())
        << "the viewer's open call line must mark a ticket active";
    ASSERT_TRUE(result->addressCallInformation.has_value())
        << "the active call's caller number must resolve to a DaviCal contact";
    EXPECT_EQ(result->addressCallInformation->name, "Alice Example");
    EXPECT_GE(davSrv.count([](const MockRequest& r) { return r.method == "REPORT"; }), 1U);
}
