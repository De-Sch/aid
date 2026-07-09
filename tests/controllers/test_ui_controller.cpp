#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/HttpTypes.h>
#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "FakeAddressBook.h"
#include "FakeTicketStore.h"
#include "FakeUiNotifier.h"
#include "aid/controllers/SessionGuard.h"
#include "aid/controllers/UiController.h"
#include "aid/crosscutting/CorrelationId.h"
#include "aid/crosscutting/Logger.h"
#include "aid/plumbing/ActionResult.h"
#include "aid/plumbing/Error.h"
#include "aid/plumbing/Result.h"
#include "aid/usecases/AppendComment.h"
#include "aid/usecases/CloseTicket.h"
#include "aid/usecases/GetDashboard.h"
#include "aid/value-types/Contact.h"
#include "aid/value-types/Dashboard.h"
#include "aid/value-types/Ids.h"
#include "aid/value-types/Ticket.h"

namespace {

using aid::AddressKind;
using aid::CallId;
using aid::Contact;
using aid::DashboardEntry;
using aid::PhoneNumber;
using aid::ProjectId;
using aid::StatusId;
using aid::Ticket;
using aid::TicketId;
using aid::TicketStatus;
using aid::UserHandle;
using aid::controllers::SessionGuard;
using aid::controllers::UiController;
using aid::crosscutting::CorrelationId;
using aid::crosscutting::Logger;
using aid::fakes::FakeAddressBook;
using aid::fakes::FakeTicketStore;
using aid::fakes::FakeUiNotifier;
using aid::plumbing::Error;
using aid::plumbing::ErrorCode;
using aid::plumbing::unexpected;
using aid::usecases::AppendComment;
using aid::usecases::CloseTicket;
using aid::usecases::GetDashboard;

struct LoggerOnce {
    LoggerOnce() {
        static std::once_flag flag;
        std::call_once(flag, [] {
            Logger::initialize(aid::crosscutting::LogLevel::ERROR,
                               "/tmp/aid_ui_ctrl_test_backend.log",
                               "/tmp/aid_ui_ctrl_test_frontend.log");
        });
    }
};

// Builds a request with the viewer attribute already populated — the
// SessionGuard filter would have done this upstream in production.
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

[[nodiscard]] nlohmann::json parseBody(const drogon::HttpResponsePtr& resp) {
    return nlohmann::json::parse(resp->getBody());
}

DashboardEntry mkEntry(std::string id, std::string href, std::optional<CallId> active = {}) {
    DashboardEntry e;
    e.id = TicketId{std::move(id)};
    e.subject = "Alice";
    e.status = TicketStatus::InProgress;
    e.statusId = StatusId{"7"};
    e.callIds.push_back(CallId{"call-123"});
    e.callerNumber = PhoneNumber{"+491701234567"};
    e.href = std::move(href);
    e.projectName = "support";
    e.activeCallForViewer = std::move(active);
    e.description =
        "alice: Call start: 2026-06-05 14:23:11 (call-123)\nCustomer asked for a callback.";
    return e;
}

Contact mkContact() {
    Contact c;
    c.name = "Bob";
    c.companyName = "ACME";
    c.kind = AddressKind::Person;
    c.phoneNumbers = {PhoneNumber{"+491701234567"}};
    c.projectIds = {ProjectId{"support"}};
    return c;
}

Ticket mkTicket(TicketId id) {
    Ticket t;
    t.id = std::move(id);
    t.projectId = ProjectId{"P1"};
    t.subject = "Alice";
    t.status = TicketStatus::InProgress;
    t.callerNumber = PhoneNumber{"+491701234567"};
    return t;
}

class UiControllerTest : public ::testing::Test, public LoggerOnce {
protected:
    FakeTicketStore ts_;
    FakeAddressBook ab_;
    FakeUiNotifier ui_;
    CorrelationId cid_;
    GetDashboard dashboard_{ts_, ab_};
    AppendComment comment_{ts_, ui_};
    CloseTicket close_{ts_, ui_};
    UiController ctrl_{dashboard_, comment_, close_, cid_, Logger::instance()};

    static UserHandle alice() { return UserHandle{"alice"}; }

    drogon::HttpResponsePtr invokeDashboard(const drogon::HttpRequestPtr& req) {
        drogon::HttpResponsePtr captured;
        ctrl_.getDashboard(req, [&captured](const drogon::HttpResponsePtr& r) { captured = r; });
        return captured;
    }

    drogon::HttpResponsePtr invokeComment(const drogon::HttpRequestPtr& req, std::string ticketId) {
        drogon::HttpResponsePtr captured;
        ctrl_.postComment(
            req, [&captured](const drogon::HttpResponsePtr& r) { captured = r; },
            std::move(ticketId));
        return captured;
    }

    drogon::HttpResponsePtr invokeClose(const drogon::HttpRequestPtr& req, std::string ticketId) {
        drogon::HttpResponsePtr captured;
        ctrl_.postClose(
            req, [&captured](const drogon::HttpResponsePtr& r) { captured = r; },
            std::move(ticketId));
        return captured;
    }
};

// ---------------------------------------------------------------------------
// Dashboard
// ---------------------------------------------------------------------------

TEST_F(UiControllerTest, Dashboard_ReturnsCombinedView_WhenTicketsStubbed) {
    ts_.nextListDashboard.push_back(std::vector<DashboardEntry>{
        mkEntry("T1", "https://op.example/projects/alpha/work_packages/1"),
        mkEntry("T2", "https://op.example/projects/beta/work_packages/2", CallId{"call-A"})});
    // Active call present → use case looks up the caller; no match here.
    ab_.nextLookup.push_back(std::optional<Contact>{});

    auto resp = invokeDashboard(makeReq(drogon::Get, "", alice()));
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);

    const auto body = parseBody(resp);
    ASSERT_TRUE(body.contains("tickets"));
    ASSERT_EQ(body["tickets"].size(), 2U);
    EXPECT_EQ(body["tickets"][0]["id"], "T1");
    EXPECT_EQ(body["tickets"][1]["id"], "T2");
    // statusId (raw OpenProject id) is carried through to the JSON contract.
    EXPECT_EQ(body["tickets"][0]["statusId"], "7");
    // description (the ticket's comment/history section) round-trips verbatim.
    EXPECT_EQ(body["tickets"][0]["description"],
              "alice: Call start: 2026-06-05 14:23:11 (call-123)\nCustomer asked for a callback.");

    ASSERT_TRUE(body.contains("active"));
    EXPECT_FALSE(body["active"].is_null());
    EXPECT_EQ(body["active"]["ticketId"], "T2");
    EXPECT_EQ(body["active"]["callId"], "call-A");
    EXPECT_EQ(body["active"]["projectName"], "beta");

    ASSERT_TRUE(body.contains("addressCallInformation"));
    EXPECT_TRUE(body["addressCallInformation"].is_null());
}

TEST_F(UiControllerTest, Dashboard_SerializesContact_WhenActiveCallMatches) {
    ts_.nextListDashboard.push_back(std::vector<DashboardEntry>{
        mkEntry("T1", "https://op.example/projects/support/work_packages/1", CallId{"call-A"})});
    ab_.nextLookup.push_back(std::optional<Contact>{mkContact()});

    auto resp = invokeDashboard(makeReq(drogon::Get, "", alice()));
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);

    const auto body = parseBody(resp);
    ASSERT_TRUE(body["addressCallInformation"].is_object());
    const auto& aci = body["addressCallInformation"];
    EXPECT_EQ(aci["name"], "Bob");
    EXPECT_EQ(aci["companyName"], "ACME");
    EXPECT_EQ(aci["kind"], "Person");
    ASSERT_TRUE(aci["phoneNumbers"].is_array());
    ASSERT_EQ(aci["phoneNumbers"].size(), 1U);
    EXPECT_EQ(aci["phoneNumbers"][0], "+491701234567");
    ASSERT_TRUE(aci["projectIds"].is_array());
    ASSERT_EQ(aci["projectIds"].size(), 1U);
    EXPECT_EQ(aci["projectIds"][0], "support");
}

TEST_F(UiControllerTest, Dashboard_AddressInfoNull_WhenActiveCallHasNoMatch) {
    ts_.nextListDashboard.push_back(std::vector<DashboardEntry>{
        mkEntry("T1", "https://op.example/projects/support/work_packages/1", CallId{"call-A"})});
    ab_.nextLookup.push_back(std::optional<Contact>{}); // clean "no match"

    auto resp = invokeDashboard(makeReq(drogon::Get, "", alice()));
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);

    const auto body = parseBody(resp);
    EXPECT_TRUE(body["addressCallInformation"].is_null());
}

TEST_F(UiControllerTest, Dashboard_Returns500_WhenViewerAttributeMissing) {
    auto resp = invokeDashboard(makeReq(drogon::Get, "", std::nullopt));
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->getStatusCode(), drogon::k500InternalServerError);
    EXPECT_EQ(parseBody(resp)["error"], "unauthenticated");
    EXPECT_TRUE(ts_.listDashboard_args.empty());
}

TEST_F(UiControllerTest, Dashboard_Returns502_WhenUpstreamUnavailable) {
    // Behaviour change: UiController used to collapse every usecase error to
    // 500; it now routes the domain ErrorCode through httpStatusForError, so an
    // upstream-down failure surfaces as 502 Bad Gateway (we are a gateway to
    // OpenProject). Body stays the generic {"error":"internal"}.
    ts_.nextListDashboard.push_back(
        unexpected{Error{ErrorCode::UpstreamUnavailable, "openproject down", std::nullopt}});

    auto resp = invokeDashboard(makeReq(drogon::Get, "", alice()));
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->getStatusCode(), drogon::k502BadGateway);
    EXPECT_EQ(parseBody(resp)["error"], "internal");
}

TEST_F(UiControllerTest, Dashboard_Returns500_WhenUsecaseErrorIsInternal) {
    // Codes with no dedicated HTTP status still collapse to 500.
    ts_.nextListDashboard.push_back(
        unexpected{Error{ErrorCode::InvariantViolation, "bug", std::nullopt}});

    auto resp = invokeDashboard(makeReq(drogon::Get, "", alice()));
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->getStatusCode(), drogon::k500InternalServerError);
    EXPECT_EQ(parseBody(resp)["error"], "internal");
}

TEST_F(UiControllerTest, Dashboard_Returns404_WhenUsecaseNotFound) {
    // Proves finishOk routes the code through the mapper rather than hardcoding.
    ts_.nextListDashboard.push_back(
        unexpected{Error{ErrorCode::NotFound, "nothing here", std::nullopt}});

    auto resp = invokeDashboard(makeReq(drogon::Get, "", alice()));
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->getStatusCode(), drogon::k404NotFound);
    EXPECT_EQ(parseBody(resp)["error"], "internal");
}

// ---------------------------------------------------------------------------
// Comment
// ---------------------------------------------------------------------------

TEST_F(UiControllerTest, Comment_Returns200WithOkTrue_OnHappyPath) {
    ts_.nextFetchById.push_back(mkTicket(TicketId{"7"}));
    ts_.nextSave.push_back(mkTicket(TicketId{"7"}));

    auto req = makeReq(drogon::Post, R"({"comment":"hello there"})", alice());
    auto resp = invokeComment(req, "7");
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);

    const auto body = parseBody(resp);
    EXPECT_TRUE(body["ok"].get<bool>());
    EXPECT_EQ(body["op"], "COMMENT_SAVE");
    EXPECT_EQ(body["ticketId"], "7");
    EXPECT_TRUE(body["message"].is_null());

    ASSERT_EQ(ui_.actionResults.size(), 1U);
    EXPECT_TRUE(ui_.actionResults[0].second.ok);
}

TEST_F(UiControllerTest, Comment_Returns200WithOkFalse_OnEmptyText) {
    auto req = makeReq(drogon::Post, R"({"comment":"   "})", alice());
    auto resp = invokeComment(req, "7");
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);

    const auto body = parseBody(resp);
    EXPECT_FALSE(body["ok"].get<bool>());
    EXPECT_EQ(body["message"], "empty comment");
    EXPECT_TRUE(ts_.fetchById_args.empty());
}

TEST_F(UiControllerTest, Comment_Returns200WithOkFalse_WhenTicketNotFound) {
    ts_.nextFetchById.push_back(
        unexpected{Error{ErrorCode::NotFound, "no such ticket", std::nullopt}});

    auto req = makeReq(drogon::Post, R"({"comment":"hi"})", alice());
    auto resp = invokeComment(req, "7");
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);

    const auto body = parseBody(resp);
    EXPECT_FALSE(body["ok"].get<bool>());
    EXPECT_EQ(body["message"], "ticket not found");
}

TEST_F(UiControllerTest, Comment_Returns400_OnMalformedBody) {
    auto req = makeReq(drogon::Post, "not json", alice());
    auto resp = invokeComment(req, "7");
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->getStatusCode(), drogon::k400BadRequest);
    EXPECT_EQ(parseBody(resp)["error"], "bad request");
    EXPECT_TRUE(ts_.fetchById_args.empty());
}

TEST_F(UiControllerTest, Comment_Returns400_WhenCommentFieldMissing) {
    auto req = makeReq(drogon::Post, "{}", alice());
    auto resp = invokeComment(req, "7");
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->getStatusCode(), drogon::k400BadRequest);
    EXPECT_TRUE(ts_.fetchById_args.empty());
}

TEST_F(UiControllerTest, Comment_Returns404_OnInvalidTicketIdShape) {
    auto req = makeReq(drogon::Post, R"({"comment":"hi"})", alice());
    auto resp = invokeComment(req, "abc");
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->getStatusCode(), drogon::k404NotFound);
    EXPECT_TRUE(ts_.fetchById_args.empty());
}

TEST_F(UiControllerTest, Comment_Returns404_OnTicketIdTooLong) {
    auto req = makeReq(drogon::Post, R"({"comment":"hi"})", alice());
    auto resp = invokeComment(req, "1234567890123");
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->getStatusCode(), drogon::k404NotFound);
}

TEST_F(UiControllerTest, Comment_Returns500_WhenViewerMissing) {
    auto req = makeReq(drogon::Post, R"({"comment":"hi"})", std::nullopt);
    auto resp = invokeComment(req, "7");
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->getStatusCode(), drogon::k500InternalServerError);
    EXPECT_EQ(parseBody(resp)["error"], "unauthenticated");
}

// ---------------------------------------------------------------------------
// Close
// ---------------------------------------------------------------------------

TEST_F(UiControllerTest, Close_Returns200WithOkTrue_OnHappyPath) {
    ts_.nextClose.push_back(aid::plumbing::Result<void>{});

    auto resp = invokeClose(makeReq(drogon::Post, "", alice()), "7");
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);

    const auto body = parseBody(resp);
    EXPECT_TRUE(body["ok"].get<bool>());
    EXPECT_EQ(body["op"], "TICKET_CLOSE");
    EXPECT_EQ(body["ticketId"], "7");
    EXPECT_TRUE(body["message"].is_null());
}

TEST_F(UiControllerTest, Close_Returns200WithOkTrue_OnIdempotentClose) {
    // Adapter returns success for already-closed tickets (path is empty,
    // no-op). From the controller's perspective this is just success.
    ts_.nextClose.push_back(aid::plumbing::Result<void>{});

    auto resp = invokeClose(makeReq(drogon::Post, "", alice()), "42");
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);
    EXPECT_TRUE(parseBody(resp)["ok"].get<bool>());
}

TEST_F(UiControllerTest, Close_Returns200WithOkFalse_WhenNotFound) {
    ts_.nextClose.push_back(unexpected{Error{ErrorCode::NotFound, "no such ticket", std::nullopt}});

    auto resp = invokeClose(makeReq(drogon::Post, "", alice()), "7");
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);

    const auto body = parseBody(resp);
    EXPECT_FALSE(body["ok"].get<bool>());
    EXPECT_EQ(body["message"], "ticket not found");
}

TEST_F(UiControllerTest, Close_Returns200WithOkFalse_WhenUpstreamFails) {
    ts_.nextClose.push_back(
        unexpected{Error{ErrorCode::LockVersionExhausted, "5 retries", std::nullopt}});

    auto resp = invokeClose(makeReq(drogon::Post, "", alice()), "7");
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);

    const auto body = parseBody(resp);
    EXPECT_FALSE(body["ok"].get<bool>());
    ASSERT_TRUE(body["message"].is_string());
    EXPECT_NE(body["message"].get<std::string>().find("close failed"), std::string::npos);
}

TEST_F(UiControllerTest, Close_Returns404_OnInvalidTicketIdShape) {
    auto resp = invokeClose(makeReq(drogon::Post, "", alice()), "abc");
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->getStatusCode(), drogon::k404NotFound);
    EXPECT_TRUE(ts_.closed.empty());
}

// ---------------------------------------------------------------------------
// JSON shape smoke
// ---------------------------------------------------------------------------

TEST_F(UiControllerTest, JsonShape_DashboardEntryHasAllFields) {
    DashboardEntry e;
    e.id = TicketId{"99"};
    e.subject = "Bob";
    e.status = TicketStatus::New;
    e.statusId = StatusId{"1"};
    e.callIds = {CallId{"c-1"}, CallId{"c-2"}};
    e.callerNumber = PhoneNumber{"+490"};
    e.calledNumber = PhoneNumber{"+491"};
    e.assignee = UserHandle{"alice"};
    e.href = "https://op.example/projects/zeta/work_packages/99";
    e.projectName = "zeta";
    e.activeCallForViewer = CallId{"c-1"};
    e.otherActiveUsers = {UserHandle{"dia"}, UserHandle{"tom"}};
    // callStart/callEnd serialize as the daemon's LOCAL wall-clock
    // "YYYY-MM-DD HH:MM:SS" (FINDING 4) — the same value stored in OpenProject,
    // shown verbatim. Pin TZ and build the instants via mktime from local
    // fields so the round-trip is deterministic regardless of the host zone.
    ::setenv("TZ", "Europe/Berlin", 1);
    ::tzset();
    std::tm st{};
    st.tm_year = 2026 - 1900;
    st.tm_mon = 6 - 1;
    st.tm_mday = 8;
    st.tm_hour = 13;
    st.tm_min = 21;
    st.tm_sec = 57;
    st.tm_isdst = -1;
    e.callStart = std::chrono::system_clock::from_time_t(::mktime(&st));
    std::tm et = st;
    et.tm_min = 23;
    et.tm_sec = 28;
    et.tm_isdst = -1;
    e.callEnd = std::chrono::system_clock::from_time_t(::mktime(&et));
    // updatedAt serializes as ISO-8601 UTC (machine field, sort key only) —
    // 1705314600 == 2024-01-15T10:30:00Z, independent of the host TZ above.
    e.updatedAt = std::chrono::system_clock::from_time_t(1705314600);
    ts_.nextListDashboard.push_back(std::vector<DashboardEntry>{e});
    ab_.nextLookup.push_back(std::optional<Contact>{}); // active call → lookup, no match

    auto resp = invokeDashboard(makeReq(drogon::Get, "", alice()));
    ASSERT_NE(resp, nullptr);
    const auto body = parseBody(resp);
    ASSERT_EQ(body["tickets"].size(), 1U);
    const auto& je = body["tickets"][0];

    EXPECT_EQ(je["id"], "99");
    EXPECT_EQ(je["subject"], "Bob");
    EXPECT_EQ(je["status"], "New");
    EXPECT_EQ(je["statusId"], "1");
    ASSERT_TRUE(je["callIds"].is_array());
    EXPECT_EQ(je["callIds"][0], "c-1");
    EXPECT_EQ(je["callIds"][1], "c-2");
    EXPECT_EQ(je["callerNumber"], "+490");
    EXPECT_EQ(je["calledNumber"], "+491");
    EXPECT_EQ(je["assignee"], "alice");
    EXPECT_EQ(je["href"], "https://op.example/projects/zeta/work_packages/99");
    EXPECT_EQ(je["projectName"], "zeta");
    EXPECT_EQ(je["activeCallForViewer"], "c-1");
    EXPECT_EQ(je["callStart"], "2026-06-08 13:21:57");
    EXPECT_EQ(je["callEnd"], "2026-06-08 13:23:28");
    EXPECT_EQ(je["updatedAt"], "2024-01-15T10:30:00Z");
    ASSERT_TRUE(je["otherActiveUsers"].is_array());
    ASSERT_EQ(je["otherActiveUsers"].size(), 2U);
    EXPECT_EQ(je["otherActiveUsers"][0], "dia");
    EXPECT_EQ(je["otherActiveUsers"][1], "tom");
}

TEST_F(UiControllerTest, JsonShape_DashboardEntryOptionalsAreNull) {
    DashboardEntry e;
    e.id = TicketId{"99"};
    e.subject = "Bob";
    e.status = TicketStatus::Closed;
    e.statusId = StatusId{"5"};
    e.callerNumber = PhoneNumber{"+490"};
    e.href = "h";
    // calledNumber, assignee, activeCallForViewer all default = nullopt.
    ts_.nextListDashboard.push_back(std::vector<DashboardEntry>{e});

    auto resp = invokeDashboard(makeReq(drogon::Get, "", alice()));
    ASSERT_NE(resp, nullptr);
    const auto body = parseBody(resp);
    const auto& je = body["tickets"][0];
    EXPECT_TRUE(je["calledNumber"].is_null());
    EXPECT_TRUE(je["assignee"].is_null());
    EXPECT_TRUE(je["callStart"].is_null());
    EXPECT_TRUE(je["callEnd"].is_null());
    EXPECT_TRUE(je["activeCallForViewer"].is_null());
    EXPECT_TRUE(je["otherActiveUsers"].is_array());
    EXPECT_TRUE(je["otherActiveUsers"].empty());
    EXPECT_EQ(je["status"], "Closed");
}

} // namespace
