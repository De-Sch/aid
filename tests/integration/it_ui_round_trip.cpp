#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/HttpTypes.h>
#include <drogon/WebSocketConnection.h>
#include <gtest/gtest.h>

#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "FakeAddressBook.h"
#include "FakeTicketStore.h"
#include "FakeWebSocketConnection.h"
#include "IntegrationHarness.h"
#include "aid/adapters/ws/WsHubAdapter.h"
#include "aid/controllers/SessionGuard.h"
#include "aid/controllers/UiController.h"
#include "aid/controllers/UiStreamController.h"
#include "aid/crosscutting/CorrelationId.h"
#include "aid/crosscutting/Logger.h"
#include "aid/plumbing/ActionResult.h"
#include "aid/usecases/AppendComment.h"
#include "aid/usecases/CloseTicket.h"
#include "aid/usecases/GetDashboard.h"
#include "aid/value-types/Dashboard.h"
#include "aid/value-types/Ids.h"
#include "aid/value-types/Ticket.h"

namespace {

using aid::CallId;
using aid::DashboardEntry;
using aid::PhoneNumber;
using aid::ProjectId;
using aid::StatusId;
using aid::Ticket;
using aid::TicketId;
using aid::TicketStatus;
using aid::UserHandle;
using aid::adapters::ws::WsHubAdapter;
using aid::controllers::SessionGuard;
using aid::controllers::UiController;
using aid::controllers::UiStreamController;
using aid::crosscutting::CorrelationId;
using aid::crosscutting::Logger;
using aid::fakes::FakeAddressBook;
using aid::fakes::FakeTicketStore;
using aid::fakes::FakeWebSocketConnection;
using aid::tests::integration::LoggerOnce;
using aid::usecases::AppendComment;
using aid::usecases::CloseTicket;
using aid::usecases::GetDashboard;

[[nodiscard]] drogon::HttpRequestPtr makeReq(drogon::HttpMethod method, std::string_view body,
                                             std::optional<UserHandle> viewer) {
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(method);
    req->setBody(std::string{body});
    if (!body.empty()) {
        req->addHeader("Content-Type", "application/json");
    }
    if (viewer.has_value()) {
        req->attributes()->insert(SessionGuard::VIEWER_KEY, *viewer);
    }
    return req;
}

[[nodiscard]] Ticket mkTicket(TicketId id) {
    Ticket t;
    t.id = std::move(id);
    t.projectId = ProjectId{"P1"};
    t.subject = "Alice";
    t.status = TicketStatus::InProgress;
    t.callerNumber = PhoneNumber{"+491701234567"};
    return t;
}

// End-to-end /ui round-trip via a real WsHubAdapter. The fakes stand in for
// the OpenProject + DaviCal ports; the hub and stream controller are the
// production classes wired into the production-shape graph. A
// FakeWebSocketConnection plays the SvelteKit client.
class UiRoundTripE2E : public ::testing::Test {
protected:
    void SetUp() override { UiStreamController::install(hub_, Logger::instance(), cid_); }
    void TearDown() override { UiStreamController::uninstall(); }

    drogon::HttpResponsePtr invokeDashboard(const drogon::HttpRequestPtr& req) {
        drogon::HttpResponsePtr captured;
        controller_.getDashboard(req,
                                 [&captured](const drogon::HttpResponsePtr& r) { captured = r; });
        return captured;
    }

    drogon::HttpResponsePtr invokeComment(const drogon::HttpRequestPtr& req, std::string ticketId) {
        drogon::HttpResponsePtr captured;
        controller_.postComment(
            req, [&captured](const drogon::HttpResponsePtr& r) { captured = r; },
            std::move(ticketId));
        return captured;
    }

    drogon::HttpResponsePtr invokeClose(const drogon::HttpRequestPtr& req, std::string ticketId) {
        drogon::HttpResponsePtr captured;
        controller_.postClose(
            req, [&captured](const drogon::HttpResponsePtr& r) { captured = r; },
            std::move(ticketId));
        return captured;
    }

    std::shared_ptr<FakeWebSocketConnection> connectWs(const UserHandle& viewer) {
        auto conn = std::make_shared<FakeWebSocketConnection>();
        auto req = makeReq(drogon::HttpMethod::Get, "", viewer);
        wsController_.handleNewConnection(req, conn);
        return conn;
    }

    static UserHandle alice() { return UserHandle{"alice"}; }

    LoggerOnce loggerInit_{};
    FakeTicketStore ts_;
    FakeAddressBook ab_;
    CorrelationId cid_;
    WsHubAdapter hub_{Logger::instance()};
    GetDashboard dashboard_{ts_, ab_};
    AppendComment comment_{ts_, hub_};
    CloseTicket close_{ts_, hub_};
    UiController controller_{dashboard_, comment_, close_, cid_, Logger::instance()};
    UiStreamController wsController_{};
};

} // namespace

// Happy-path dashboard fetch returns the projected DashboardView through the
// real UiController → GetDashboard → FakeTicketStore chain.
TEST_F(UiRoundTripE2E, GetDashboard_HappyPath_ReturnsViewWithTickets) {
    DashboardEntry entry;
    entry.id = TicketId{"42"};
    entry.subject = "Alice";
    entry.status = TicketStatus::InProgress;
    entry.statusId = StatusId{"7"};
    entry.callIds.push_back(CallId{"call-1"});
    entry.callerNumber = PhoneNumber{"+491701234567"};
    entry.href = "https://op.example/projects/support/work_packages/42";
    ts_.nextListDashboard.push_back(std::vector<DashboardEntry>{entry});

    auto resp = invokeDashboard(makeReq(drogon::HttpMethod::Get, "", alice()));
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);

    const auto body = nlohmann::json::parse(resp->getBody());
    ASSERT_TRUE(body.contains("tickets"));
    ASSERT_EQ(body["tickets"].size(), 1U);
    EXPECT_EQ(body["tickets"][0]["id"], "42");
    EXPECT_TRUE(body.contains("active"));
    EXPECT_TRUE(body.contains("addressCallInformation"));
}

// POST /ui/comment lands an action_result frame on the viewer's WS and a
// per-recipient ticket_upsert delta (the post-save ticket, still open) on each
// recipient's WS. End-to-end through the real hub.
TEST_F(UiRoundTripE2E, PostComment_PushesActionResultAndUpsert) {
    auto conn = connectWs(alice());
    ASSERT_EQ(hub_.subscriberCount(), 1U);
    ASSERT_FALSE(conn->isClosed());

    ts_.nextFetchById.push_back(mkTicket(TicketId{"42"}));
    ts_.nextSave.push_back(mkTicket(TicketId{"42"}));
    ts_.nextRecipientsFor.push_back(
        aid::plumbing::Result<std::vector<UserHandle>>{std::vector<UserHandle>{alice()}});

    auto resp =
        invokeComment(makeReq(drogon::HttpMethod::Post, R"({"comment":"hello"})", alice()), "42");
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);

    const auto body = nlohmann::json::parse(resp->getBody());
    EXPECT_EQ(body["op"], "COMMENT_SAVE");
    EXPECT_EQ(body["ok"], true);
    EXPECT_EQ(body["ticketId"], "42");

    const auto sent = conn->sent();
    ASSERT_EQ(sent.size(), 2U);

    bool sawActionResult = false;
    bool sawUpsert = false;
    for (const auto& frame : sent) {
        const auto j = nlohmann::json::parse(frame);
        if (j["type"] == "action_result") {
            EXPECT_EQ(j["op"], "COMMENT_SAVE");
            EXPECT_EQ(j["ok"], true);
            EXPECT_EQ(j["ticketId"], "42");
            sawActionResult = true;
        } else if (j["type"] == "ticket_upsert") {
            EXPECT_EQ(j["entry"]["id"], "42");
            sawUpsert = true;
        }
    }
    EXPECT_TRUE(sawActionResult);
    EXPECT_TRUE(sawUpsert);
}

// POST /ui/close fires the TICKET_CLOSE action_result + a ticket_remove delta
// (the now-Closed ticket leaves every recipient's board).
TEST_F(UiRoundTripE2E, PostClose_PushesActionResultAndRemove) {
    auto conn = connectWs(alice());
    ASSERT_EQ(hub_.subscriberCount(), 1U);

    ts_.nextClose.push_back(aid::plumbing::Result<void>{});
    // After a successful close the use case re-fetches the now-Closed ticket.
    Ticket closed = mkTicket(TicketId{"42"});
    closed.status = TicketStatus::Closed;
    closed.lockVersion = 3;
    ts_.nextFetchById.push_back(closed);
    ts_.nextRecipientsFor.push_back(
        aid::plumbing::Result<std::vector<UserHandle>>{std::vector<UserHandle>{alice()}});

    auto resp = invokeClose(makeReq(drogon::HttpMethod::Post, "", alice()), "42");
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);

    const auto body = nlohmann::json::parse(resp->getBody());
    EXPECT_EQ(body["op"], "TICKET_CLOSE");
    EXPECT_EQ(body["ok"], true);

    const auto sent = conn->sent();
    ASSERT_EQ(sent.size(), 2U);

    bool sawActionResult = false;
    bool sawRemove = false;
    for (const auto& frame : sent) {
        const auto j = nlohmann::json::parse(frame);
        if (j["type"] == "action_result") {
            EXPECT_EQ(j["op"], "TICKET_CLOSE");
            sawActionResult = true;
        } else if (j["type"] == "ticket_remove") {
            EXPECT_EQ(j["ticketId"], "42");
            EXPECT_EQ(j["lockVersion"], 3);
            sawRemove = true;
        }
    }
    EXPECT_TRUE(sawActionResult);
    EXPECT_TRUE(sawRemove);
}

// The delta protocol is targeted, not broadcast: a connected user who is NOT a
// recipient of the ticket (not a project member / call handler) receives
// nothing. Here recipientsFor returns only alice, so bob — though subscribed —
// gets no frame at all, while alice gets action_result + ticket_upsert.
TEST_F(UiRoundTripE2E, PostComment_TargetsRecipientsOnly_StrangerGetsNothing) {
    auto alicesConn = connectWs(alice());
    auto bobsConn = connectWs(UserHandle{"bob"});
    ASSERT_EQ(hub_.subscriberCount(), 2U);

    ts_.nextFetchById.push_back(mkTicket(TicketId{"42"}));
    ts_.nextSave.push_back(mkTicket(TicketId{"42"}));
    ts_.nextRecipientsFor.push_back(
        aid::plumbing::Result<std::vector<UserHandle>>{std::vector<UserHandle>{alice()}});

    auto resp =
        invokeComment(makeReq(drogon::HttpMethod::Post, R"({"comment":"hi"})", alice()), "42");
    ASSERT_EQ(resp->getStatusCode(), drogon::k200OK);

    EXPECT_EQ(alicesConn->sent().size(), 2U); // action_result + ticket_upsert
    EXPECT_EQ(bobsConn->sent().size(), 0U);   // not a recipient → no delta
}

// 12-char ticketId regex (^[0-9]{1,12}$) is the contract; non-numeric input
// short-circuits with 404 before reaching the use case.
TEST_F(UiRoundTripE2E, PostComment_NonNumericTicketId_Returns404) {
    auto resp = invokeComment(makeReq(drogon::HttpMethod::Post, R"({"comment":"x"})", alice()),
                              "not-a-number");
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->getStatusCode(), drogon::k404NotFound);
}
