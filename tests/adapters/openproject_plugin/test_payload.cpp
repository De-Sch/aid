#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

#include "aid/adapters/openproject/internal/CustomFieldMap.h"
#include "aid/adapters/openproject/internal/OpStatusMap.h"
#include "aid/adapters/openproject/internal/payload.h"
#include "aid/crosscutting/Config.h"
#include "aid/plumbing/Error.h"
#include "aid/value-types/Ticket.h"

using aid::adapters::openproject::CustomFieldMap;
using aid::adapters::openproject::OpStatusMap;
using aid::adapters::openproject::parseFromHal;
using aid::adapters::openproject::toCreatePayload;
using aid::adapters::openproject::toPatchPayload;
using aid::crosscutting::TicketSystemConfig;
using aid::plumbing::ErrorCode;
using json = nlohmann::json;

namespace {

TicketSystemConfig sampleCfg() {
    TicketSystemConfig cfg;
    cfg.baseUrl = "http://op.example.com";
    cfg.apiToken = "t";
    cfg.statusNew = aid::StatusId{"1"};
    cfg.statusInProgress = aid::StatusId{"2"};
    cfg.statusClosed = aid::StatusId{"3"};
    cfg.typeCall = "7";
    return cfg;
}

CustomFieldMap sampleFields() {
    return CustomFieldMap{aid::CustomFieldId{"1"}, aid::CustomFieldId{"2"}, aid::CustomFieldId{"3"},
                          aid::CustomFieldId{"4"}, aid::CustomFieldId{"5"}, aid::CustomFieldId{"6"},
                          aid::CustomFieldId{"7"}};
}

// Build a representative HAL response. Tests can mutate before calling
// parseFromHal to exercise edge cases.
json halBody() {
    // Custom delimiter: the call-log line ends with `(call-1)"`, whose `)"`
    // would otherwise terminate a plain R"(...)" raw string early.
    return json::parse(R"json({
        "id": 42,
        "subject": "Call from +491234",
        "description": { "format": "markdown", "raw": "Body text" },
        "lockVersion": 3,
        "updatedAt": "2024-01-15T10:30:00Z",
        "customField1": "call-1,call-2",
        "customField2": "+491234",
        "customField3": "+490987",
        "customField4": "2024-01-15 10:30:00",
        "customField5": "2024-01-15 10:31:00",
        "customField6": { "format": "markdown", "raw": "alice: Call start: 2024-01-15 10:30:00 (call-1)" },
        "customField7": { "format": "markdown", "raw": "alice, bob" },
        "_links": {
            "self":     { "href": "/api/v3/work_packages/42" },
            "project":  { "href": "/api/v3/projects/11", "title": "Acme" },
            "status":   { "href": "/api/v3/statuses/2",  "title": "In Progress" },
            "assignee": { "href": "/api/v3/users/9",     "title": "Alice" },
            "type":     { "href": "/api/v3/types/7",     "title": "Call" }
        },
        "_embedded": {
    "assignee" : {
        "id" : 9, "login" : "alice", "name" : "Alice Smith"
    }
}
})json");
}

} // namespace

// ─── parseFromHal ────────────────────────────────────────────────────────

TEST(ParseFromHal, HappyPathExtractsAllFields) {
    const auto m = OpStatusMap::fromConfig(sampleCfg());
    auto r = parseFromHal(halBody(), sampleFields(), m);
    ASSERT_TRUE(r.has_value()) << r.error().message;

    EXPECT_EQ(r->id.v, "42");
    EXPECT_EQ(r->subject, "Call from +491234");
    EXPECT_EQ(r->description, "Body text");
    EXPECT_EQ(r->lockVersion, 3);
    EXPECT_EQ(r->projectId.v, "11");
    EXPECT_EQ(r->status, aid::TicketStatus::InProgress);
    EXPECT_EQ(r->statusId.v, "2"); // raw OpenProject status id carried verbatim
    ASSERT_TRUE(r->assignee.has_value());
    EXPECT_EQ(r->assignee->v, "alice"); // _embedded.assignee.login preferred over href tail
    ASSERT_EQ(r->callIds.size(), 2U);
    EXPECT_EQ(r->callIds[0].v, "call-1");
    EXPECT_EQ(r->callIds[1].v, "call-2");
    EXPECT_EQ(r->callerNumber.v, "+491234");
    ASSERT_TRUE(r->calledNumber.has_value());
    EXPECT_EQ(r->calledNumber->v, "+490987");
    ASSERT_TRUE(r->callStart.has_value());
    ASSERT_TRUE(r->callEnd.has_value());
    // callLength is a Formattable field → read from its .raw.
    EXPECT_EQ(r->callLength, "alice: Call start: 2024-01-15 10:30:00 (call-1)");
    // callHandler is a Formattable field → ", "-CSV split into logins.
    ASSERT_EQ(r->callHandlers.size(), 2U);
    EXPECT_EQ(r->callHandlers[0].v, "alice");
    EXPECT_EQ(r->callHandlers[1].v, "bob");
}

TEST(ParseFromHal, CallHandlersWhitespaceTolerantSplit) {
    const auto m = OpStatusMap::fromConfig(sampleCfg());
    auto hal = halBody();
    hal["customField7"]["raw"] = "a, b ,c,, d";
    auto r = parseFromHal(hal, sampleFields(), m);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    ASSERT_EQ(r->callHandlers.size(), 4U);
    EXPECT_EQ(r->callHandlers[0].v, "a");
    EXPECT_EQ(r->callHandlers[1].v, "b");
    EXPECT_EQ(r->callHandlers[2].v, "c");
    EXPECT_EQ(r->callHandlers[3].v, "d");
}

TEST(ParseFromHal, MissingCallHandlerLeavesEmptyVector) {
    const auto m = OpStatusMap::fromConfig(sampleCfg());
    auto hal = halBody();
    hal.erase("customField7");
    auto r = parseFromHal(hal, sampleFields(), m);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_TRUE(r->callHandlers.empty());
}

TEST(ParseFromHal, EmptyCallHandlerRawLeavesEmptyVector) {
    const auto m = OpStatusMap::fromConfig(sampleCfg());
    auto hal = halBody();
    hal["customField7"]["raw"] = "";
    auto r = parseFromHal(hal, sampleFields(), m);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_TRUE(r->callHandlers.empty());
}

TEST(ParseFromHal, MissingDescriptionLeavesEmptyString) {
    const auto m = OpStatusMap::fromConfig(sampleCfg());
    auto hal = halBody();
    hal.erase("description");
    auto r = parseFromHal(hal, sampleFields(), m);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_TRUE(r->description.empty());
}

TEST(ParseFromHal, MissingCalledNumberLeavesNullopt) {
    const auto m = OpStatusMap::fromConfig(sampleCfg());
    auto hal = halBody();
    hal["customField3"] = nullptr;
    auto r = parseFromHal(hal, sampleFields(), m);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_FALSE(r->calledNumber.has_value());
}

TEST(ParseFromHal, MissingAssigneeLeavesNullopt) {
    const auto m = OpStatusMap::fromConfig(sampleCfg());
    auto hal = halBody();
    hal["_links"].erase("assignee");
    hal["_embedded"].erase("assignee");
    auto r = parseFromHal(hal, sampleFields(), m);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_FALSE(r->assignee.has_value());
}

TEST(ParseFromHal, AssigneeWithoutEmbeddedFallsBackToLinkTitle) {
    // No embedded login → use the link's display-name title ("Alice"),
    // not the bare numeric href tail.
    const auto m = OpStatusMap::fromConfig(sampleCfg());
    auto hal = halBody();
    hal.erase("_embedded");
    auto r = parseFromHal(hal, sampleFields(), m);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    ASSERT_TRUE(r->assignee.has_value());
    EXPECT_EQ(r->assignee->v, "Alice");
}

TEST(ParseFromHal, AssigneeWithoutEmbeddedOrTitleFallsBackToHrefTail) {
    const auto m = OpStatusMap::fromConfig(sampleCfg());
    auto hal = halBody();
    hal.erase("_embedded");
    hal["_links"]["assignee"].erase("title");
    auto r = parseFromHal(hal, sampleFields(), m);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    ASSERT_TRUE(r->assignee.has_value());
    EXPECT_EQ(r->assignee->v, "9");
}

TEST(ParseFromHal, UnconfiguredStatusKeepsRawIdButEnumDefaultsToNew) {
    // The raw statusId is captured verbatim even when the id is not one of
    // the configured statuses — the whole point of carrying it separately
    // from the lossy enum collapse (statusFor() defaults unknown ids to New).
    const auto m = OpStatusMap::fromConfig(sampleCfg());
    auto hal = halBody();
    hal["_links"]["status"]["href"] = "/api/v3/statuses/99";
    auto r = parseFromHal(hal, sampleFields(), m);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->statusId.v, "99");
    EXPECT_EQ(r->status, aid::TicketStatus::New);
}

TEST(ParseFromHal, MultipleCallIdsCommaSplit) {
    const auto m = OpStatusMap::fromConfig(sampleCfg());
    auto hal = halBody();
    hal["customField1"] = "a, b ,c,, d";
    auto r = parseFromHal(hal, sampleFields(), m);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    ASSERT_EQ(r->callIds.size(), 4U);
    EXPECT_EQ(r->callIds[0].v, "a");
    EXPECT_EQ(r->callIds[1].v, "b");
    EXPECT_EQ(r->callIds[2].v, "c");
    EXPECT_EQ(r->callIds[3].v, "d");
}

TEST(ParseFromHal, EmptyCallIdsLeavesEmptyVector) {
    const auto m = OpStatusMap::fromConfig(sampleCfg());
    auto hal = halBody();
    hal["customField1"] = "";
    auto r = parseFromHal(hal, sampleFields(), m);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_TRUE(r->callIds.empty());
}

TEST(ParseFromHal, MissingLockVersionIsRejected) {
    const auto m = OpStatusMap::fromConfig(sampleCfg());
    auto hal = halBody();
    hal.erase("lockVersion");
    auto r = parseFromHal(hal, sampleFields(), m);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ErrorCode::InvalidInput);
    EXPECT_NE(r.error().message.find("lockVersion"), std::string::npos);
}

TEST(ParseFromHal, MissingProjectLinkIsRejected) {
    const auto m = OpStatusMap::fromConfig(sampleCfg());
    auto hal = halBody();
    hal["_links"].erase("project");
    auto r = parseFromHal(hal, sampleFields(), m);
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().message.find("project"), std::string::npos);
}

TEST(ParseFromHal, IsoTimestampParsesToUtc) {
    const auto m = OpStatusMap::fromConfig(sampleCfg());
    auto r = parseFromHal(halBody(), sampleFields(), m);
    ASSERT_TRUE(r.has_value()) << r.error().message;

    // "2024-01-15T10:30:00Z" — independent of host time zone. timegm()
    // reads tm fields as UTC.
    std::tm tm{};
    tm.tm_year = 124;
    tm.tm_mon = 0;
    tm.tm_mday = 15;
    tm.tm_hour = 10;
    tm.tm_min = 30;
    tm.tm_sec = 0;
    const auto expected = std::chrono::system_clock::from_time_t(::timegm(&tm));
    EXPECT_EQ(r->updatedAt, expected);
}

TEST(ParseFromHal, RejectsTopLevelNotAnObject) {
    const auto m = OpStatusMap::fromConfig(sampleCfg());
    auto r = parseFromHal(json::array({1, 2}), sampleFields(), m);
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().message.find("top-level"), std::string::npos);
}

// ─── toCreatePayload ──────────────────────────────────────────────────────

TEST(ToCreatePayload, IncludesAllRequiredFieldsWithNumericCustomKeys) {
    const auto m = OpStatusMap::fromConfig(sampleCfg());
    aid::NewTicket nt;
    nt.projectId = aid::ProjectId{"11"};
    nt.subject = "Call from +491234";
    nt.status = aid::TicketStatus::New;
    nt.callId = aid::CallId{"call-1"};
    nt.callerNumber = aid::PhoneNumber{"+491234"};
    nt.calledNumber = aid::PhoneNumber{"+490987"};

    const auto body = toCreatePayload(nt, sampleFields(), m, sampleCfg());

    EXPECT_EQ(body["subject"], "Call from +491234");
    EXPECT_EQ(body["customField1"], "call-1");
    EXPECT_EQ(body["customField2"], "+491234");
    EXPECT_EQ(body["customField3"], "+490987");
    EXPECT_EQ(body["_links"]["status"]["href"], "/api/v3/statuses/1");
    EXPECT_EQ(body["_links"]["type"]["href"], "/api/v3/types/7");
    EXPECT_FALSE(body["_links"].contains("assignee"));
}

TEST(ToCreatePayload, OmitsCalledNumberWhenUnset) {
    const auto m = OpStatusMap::fromConfig(sampleCfg());
    aid::NewTicket nt;
    nt.projectId = aid::ProjectId{"11"};
    nt.subject = "x";
    nt.callId = aid::CallId{"c"};
    nt.callerNumber = aid::PhoneNumber{"+491234"};

    const auto body = toCreatePayload(nt, sampleFields(), m, sampleCfg());
    EXPECT_FALSE(body.contains("customField3"));
}

TEST(ToCreatePayload, IncludesResolvedAssigneeHrefWhenProvided) {
    const auto m = OpStatusMap::fromConfig(sampleCfg());
    aid::NewTicket nt;
    nt.projectId = aid::ProjectId{"11"};
    nt.subject = "x";
    nt.callId = aid::CallId{"c"};
    nt.callerNumber = aid::PhoneNumber{"+1"};
    nt.assignee = aid::UserHandle{"alice"};

    // The caller resolves the login → numeric user href; the payload must
    // carry that verbatim, NOT "/api/v3/users/alice" (OpenProject 422s the
    // login form).
    const auto body = toCreatePayload(nt, sampleFields(), m, sampleCfg(),
                                      std::optional<std::string>{"/api/v3/users/9"});
    EXPECT_EQ(body["_links"]["assignee"]["href"], "/api/v3/users/9");
}

TEST(ToCreatePayload, OmitsAssigneeWhenHrefUnresolved) {
    const auto m = OpStatusMap::fromConfig(sampleCfg());
    aid::NewTicket nt;
    nt.projectId = aid::ProjectId{"11"};
    nt.subject = "x";
    nt.callId = aid::CallId{"c"};
    nt.callerNumber = aid::PhoneNumber{"+1"};
    nt.assignee = aid::UserHandle{"alice"};

    // assignee present on the ticket but no resolved href → omit the link
    // rather than emit a rejected login-form href.
    const auto body = toCreatePayload(nt, sampleFields(), m, sampleCfg());
    EXPECT_FALSE(body["_links"].contains("assignee"));
}

// ─── toPatchPayload ──────────────────────────────────────────────────────

TEST(ToPatchPayload, AlwaysIncludesLockVersion) {
    const auto m = OpStatusMap::fromConfig(sampleCfg());
    aid::Ticket t;
    t.id = aid::TicketId{"42"};
    t.projectId = aid::ProjectId{"11"};
    t.subject = "x";
    t.status = aid::TicketStatus::InProgress;
    t.lockVersion = 7;
    t.callerNumber = aid::PhoneNumber{"+491234"};

    const auto body = toPatchPayload(t, sampleFields(), m);
    EXPECT_EQ(body["lockVersion"], 7);
    EXPECT_EQ(body["_links"]["status"]["href"], "/api/v3/statuses/2");
}

TEST(ToPatchPayload, ClosingStatusUsesConfiguredHref) {
    const auto m = OpStatusMap::fromConfig(sampleCfg());
    aid::Ticket t;
    t.id = aid::TicketId{"42"};
    t.projectId = aid::ProjectId{"11"};
    t.lockVersion = 1;
    t.status = aid::TicketStatus::Closed;
    t.callerNumber = aid::PhoneNumber{"x"};

    const auto body = toPatchPayload(t, sampleFields(), m);
    EXPECT_EQ(body["_links"]["status"]["href"], "/api/v3/statuses/3");
}

TEST(ToPatchPayload, AssigneeUnsetEmitsNoAssigneeLink) {
    const auto m = OpStatusMap::fromConfig(sampleCfg());
    aid::Ticket t;
    t.id = aid::TicketId{"42"};
    t.lockVersion = 1;
    t.status = aid::TicketStatus::New;
    t.callerNumber = aid::PhoneNumber{"x"};

    const auto body = toPatchPayload(t, sampleFields(), m);
    EXPECT_FALSE(body["_links"].contains("assignee"));
}

TEST(ToPatchPayload, EmitsResolvedAssigneeHrefWhenProvided) {
    const auto m = OpStatusMap::fromConfig(sampleCfg());
    aid::Ticket t;
    t.id = aid::TicketId{"42"};
    t.lockVersion = 1;
    t.status = aid::TicketStatus::New;
    t.callerNumber = aid::PhoneNumber{"x"};
    t.assignee = aid::UserHandle{"bob"};

    // Caller-resolved numeric href is emitted verbatim, not the login form.
    const auto body =
        toPatchPayload(t, sampleFields(), m, std::optional<std::string>{"/api/v3/users/4"});
    EXPECT_EQ(body["_links"]["assignee"]["href"], "/api/v3/users/4");
}

TEST(ToPatchPayload, OmitsAssigneeWhenHrefUnresolved) {
    const auto m = OpStatusMap::fromConfig(sampleCfg());
    aid::Ticket t;
    t.id = aid::TicketId{"42"};
    t.lockVersion = 1;
    t.status = aid::TicketStatus::New;
    t.callerNumber = aid::PhoneNumber{"x"};
    t.assignee = aid::UserHandle{"bob"};

    // assignee present but no resolved href → no link (keeps the rest of the
    // patch valid instead of 422ing on a login-form href).
    const auto body = toPatchPayload(t, sampleFields(), m);
    EXPECT_FALSE(body["_links"].contains("assignee"));
}

TEST(ToPatchPayload, CallIdsJoinedWithCommas) {
    const auto m = OpStatusMap::fromConfig(sampleCfg());
    aid::Ticket t;
    t.id = aid::TicketId{"42"};
    t.lockVersion = 1;
    t.status = aid::TicketStatus::New;
    t.callerNumber = aid::PhoneNumber{"x"};
    t.callIds = {aid::CallId{"a"}, aid::CallId{"b"}, aid::CallId{"c"}};

    const auto body = toPatchPayload(t, sampleFields(), m);
    EXPECT_EQ(body["customField1"], "a,b,c");
}

TEST(ToPatchPayload, CallStartFormattedAsCustomFieldTimestamp) {
    const auto m = OpStatusMap::fromConfig(sampleCfg());
    aid::Ticket t;
    t.id = aid::TicketId{"42"};
    t.lockVersion = 1;
    t.status = aid::TicketStatus::New;
    t.callerNumber = aid::PhoneNumber{"x"};

    // callStart is serialized as the daemon's LOCAL wall-clock (FINDING 4) so
    // it matches the callLength breadcrumb and what OpenProject shows. Pin TZ
    // and build the instant via mktime from local fields so the round-trip is
    // deterministic regardless of the host zone.
    ::setenv("TZ", "Europe/Berlin", 1);
    ::tzset();
    std::tm tm{};
    tm.tm_year = 124;
    tm.tm_mon = 0;
    tm.tm_mday = 15;
    tm.tm_hour = 10;
    tm.tm_min = 30;
    tm.tm_sec = 0;
    tm.tm_isdst = -1;
    t.callStart = std::chrono::system_clock::from_time_t(::mktime(&tm));

    const auto body = toPatchPayload(t, sampleFields(), m);
    EXPECT_EQ(body["customField4"], "2024-01-15 10:30:00");
}

TEST(ToPatchPayload, CallLengthWrittenAsFormattableRaw) {
    const auto m = OpStatusMap::fromConfig(sampleCfg());
    aid::Ticket t;
    t.id = aid::TicketId{"42"};
    t.lockVersion = 1;
    t.status = aid::TicketStatus::New;
    t.callerNumber = aid::PhoneNumber{"x"};
    t.callLength = "alice: Call start: 2024-01-15 10:30:00 (call-1)";

    const auto body = toPatchPayload(t, sampleFields(), m);
    // Formattable field: emitted as a {format, raw} object, like description.
    ASSERT_TRUE(body["customField6"].is_object());
    EXPECT_EQ(body["customField6"]["format"], "markdown");
    EXPECT_EQ(body["customField6"]["raw"], "alice: Call start: 2024-01-15 10:30:00 (call-1)");
}

// toPatchPayload deliberately does NOT touch the callHandler field — that is
// owned solely by toCallHandlerPatch / addCallHandler, whose merge-on-409 loop
// would otherwise be clobbered by a plain status save writing a stale CSV.
TEST(ToPatchPayload, DoesNotWriteCallHandlerField) {
    const auto m = OpStatusMap::fromConfig(sampleCfg());
    aid::Ticket t;
    t.id = aid::TicketId{"42"};
    t.lockVersion = 1;
    t.status = aid::TicketStatus::New;
    t.callerNumber = aid::PhoneNumber{"x"};
    t.callHandlers = {aid::UserHandle{"alice"}, aid::UserHandle{"bob"}};

    const auto body = toPatchPayload(t, sampleFields(), m);
    EXPECT_FALSE(body.contains("customField7"))
        << "callHandler is written only by toCallHandlerPatch (merge-on-409 owner)";
}

// ─── toCallHandlerPatch — minimal partial body ─────────────────────────────

TEST(ToCallHandlerPatch, WritesOnlyLockVersionAndHandlerCsv) {
    const auto body =
        toCallHandlerPatch(7, {aid::UserHandle{"alice"}, aid::UserHandle{"bob"}}, sampleFields());
    EXPECT_EQ(body["lockVersion"], 7);
    ASSERT_TRUE(body["customField7"].is_object());
    EXPECT_EQ(body["customField7"]["format"], "markdown");
    EXPECT_EQ(body["customField7"]["raw"], "alice, bob");
    // Minimal partial PATCH: nothing else, so it cannot clobber other fields.
    EXPECT_FALSE(body.contains("_links"));
    EXPECT_FALSE(body.contains("subject"));
    EXPECT_FALSE(body.contains("customField6"));
}

TEST(ToCallHandlerPatch, EmptyHandlersWriteEmptyRaw) {
    const auto body = toCallHandlerPatch(2, {}, sampleFields());
    EXPECT_EQ(body["lockVersion"], 2);
    ASSERT_TRUE(body["customField7"].is_object());
    EXPECT_EQ(body["customField7"]["raw"], "");
}

// ─── round-trip ───────────────────────────────────────────────────────────

TEST(PayloadRoundTrip, CallHandlersSurviveParseThenCallHandlerPatch) {
    const auto m = OpStatusMap::fromConfig(sampleCfg());
    auto hal = halBody();
    hal["customField7"]["raw"] = "alice, bob, carol";

    auto parsed = parseFromHal(hal, sampleFields(), m);
    ASSERT_TRUE(parsed.has_value()) << parsed.error().message;
    ASSERT_EQ(parsed->callHandlers.size(), 3U);

    // Feed the parsed handlers into the dedicated callHandler PATCH; the CSV
    // must come out byte-identical.
    const auto body = toCallHandlerPatch(parsed->lockVersion, parsed->callHandlers, sampleFields());
    EXPECT_EQ(body["customField7"]["raw"], "alice, bob, carol");
}
