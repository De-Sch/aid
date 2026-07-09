#include <gtest/gtest.h>

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "aid/adapters/openproject/internal/CustomFieldMap.h"
#include "aid/adapters/openproject/internal/HandlerLedger.h"
#include "aid/adapters/openproject/internal/OpHttp.h"
#include "aid/adapters/openproject/internal/OpStatusMap.h"
#include "aid/adapters/openproject/internal/OpTicketRepo.h"
#include "aid/adapters/openproject/internal/OpUserRepo.h"
#include "aid/adapters/openproject/internal/ProducedLedger.h"
#include "aid/crosscutting/Config.h"
#include "aid/plumbing/Error.h"
#include "tests/adapters/openproject_plugin/fake_http_dispatcher.h"

using aid::adapters::openproject::CustomFieldMap;
using aid::adapters::openproject::OpHttp;
using aid::adapters::openproject::OpStatusMap;
using aid::adapters::openproject::OpTicketRepo;
using aid::adapters::openproject::OpUserRepo;
using aid::crosscutting::TicketSystemConfig;
using aid::plumbing::ErrorCode;
using aid::test_support::FakeHttpDispatcher;
using aid::test_support::FakeSleeper;
using json = nlohmann::json;

namespace {

template <class T> T drainSync(aid::plumbing::Task<T>&& task) {
    EXPECT_TRUE(task.done());
    return task.await_resume();
}

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

// Minimal HAL work_package — every find* needs a parseable body for the
// happy path. id, subject, lockVersion, _links.project/status mandatory.
std::string halTicket(const std::string& id, int lockVersion, const std::string& statusHref) {
    json j;
    j["id"] = std::stoll(id);
    j["subject"] = "Call from +491234";
    j["description"]["raw"] = "";
    j["lockVersion"] = lockVersion;
    j["updatedAt"] = "2024-01-15T10:30:00Z";
    j["customField1"] = "c-1";
    j["customField2"] = "+491234";
    j["_links"]["project"]["href"] = "/api/v3/projects/11";
    j["_links"]["status"]["href"] = statusHref;
    j["_links"]["self"]["href"] = "/api/v3/work_packages/" + id;
    return j.dump();
}

std::string halCollectionOf(const std::string& halBody) {
    json envelope;
    envelope["_embedded"]["elements"] = json::array();
    envelope["_embedded"]["elements"].push_back(json::parse(halBody));
    return envelope.dump();
}

std::string emptyCollection() {
    return R"({"_embedded":{"elements":[]}})";
}

struct Harness {
    FakeHttpDispatcher dispatcher;
    FakeSleeper sleeper;
    TicketSystemConfig cfg = sampleCfg();
    CustomFieldMap fields = sampleFields();
    OpStatusMap statusMap = OpStatusMap::fromConfig(cfg);
    OpHttp http;
    OpUserRepo users;
    aid::adapters::openproject::ProducedLedger ledger;
    aid::adapters::openproject::HandlerLedger handlerLedger;
    OpTicketRepo tickets;

    Harness()
        : http(dispatcher, "http://op.example.com", "t", sleeper.sleeper()), users(http),
          tickets(http, users, statusMap, cfg, fields, &ledger, &handlerLedger) {}
};

} // namespace

// ─── fetchById ───────────────────────────────────────────────────────────

TEST(OpTicketRepo, FetchByIdHitsCorrectPath) {
    Harness h;
    h.dispatcher.enqueueResponse(200, halTicket("42", 3, "/api/v3/statuses/2"));

    auto r = drainSync(h.tickets.fetchById(aid::TicketId{"42"}));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->id.v, "42");
    EXPECT_EQ(r->status, aid::TicketStatus::InProgress);
    ASSERT_EQ(h.dispatcher.calls().size(), 1U);
    EXPECT_EQ(h.dispatcher.calls()[0].method, "GET");
    EXPECT_EQ(h.dispatcher.calls()[0].path, "/api/v3/work_packages/42");
}

// ─── find* filter shapes ─────────────────────────────────────────────────

TEST(OpTicketRepo, FindByExactCallidUsesEqOperator) {
    Harness h;
    h.dispatcher.enqueueResponse(200, halCollectionOf(halTicket("42", 1, "/api/v3/statuses/1")));

    (void)drainSync(h.tickets.findByExactCallid(aid::CallId{"abc"}));
    ASSERT_EQ(h.dispatcher.calls().size(), 1U);
    const auto& path = h.dispatcher.calls()[0].path;
    EXPECT_NE(path.find("/api/v3/work_packages?filters="), std::string::npos);
    // "operator":"=" encoded — the colon and equals make it tricky;
    // assert on the percent-encoded operand `=` (%3D) inside JSON.
    EXPECT_NE(path.find("%3D"), std::string::npos);
}

TEST(OpTicketRepo, FindByCallidContainsUsesTildeOperator) {
    Harness h;
    h.dispatcher.enqueueResponse(200, halCollectionOf(halTicket("42", 1, "/api/v3/statuses/1")));

    (void)drainSync(h.tickets.findByCallidContains(aid::CallId{"abc"}));
    ASSERT_EQ(h.dispatcher.calls().size(), 1U);
    // "~" url-encodes to itself (unreserved). The substring "%22~%22"
    // shows the operator inside the JSON.
    const auto& path = h.dispatcher.calls()[0].path;
    EXPECT_NE(path.find("~"), std::string::npos);
}

TEST(OpTicketRepo, FindByCallidEscapesQuotesAndBackslashes) {
    Harness h;
    // Injection vector: a callid containing `"`, `]`, `\` would break out
    // of the filter operand if naively concatenated. nlohmann::json
    // escapes the JSON; urlEncode encodes %22, %5C, %5D into the URL.
    h.dispatcher.enqueueResponse(200, emptyCollection());

    (void)drainSync(h.tickets.findByCallidContains(aid::CallId{R"(a"]\b)"}));
    ASSERT_EQ(h.dispatcher.calls().size(), 1U);
    const auto& path = h.dispatcher.calls()[0].path;
    // Must contain neither a literal " nor a literal \ in the URL.
    EXPECT_EQ(path.find('"'), std::string::npos);
    EXPECT_EQ(path.find('\\'), std::string::npos);
    // %22 = ", %5C = \, %5D = ] — all three must appear.
    EXPECT_NE(path.find("%22"), std::string::npos);
    EXPECT_NE(path.find("%5C"), std::string::npos);
    EXPECT_NE(path.find("%5D"), std::string::npos);
}

TEST(OpTicketRepo, FindLatestOpenCallInProjectExcludesClosed) {
    Harness h;
    h.dispatcher.enqueueResponse(200, emptyCollection());

    (void)drainSync(h.tickets.findLatestOpenCallInProject(aid::ProjectId{"11"}));
    ASSERT_EQ(h.dispatcher.calls().size(), 1U);
    const auto& path = h.dispatcher.calls()[0].path;
    // "!" is the "is not" operator in OpenProject (the only valid spelling
    // of status!=Closed; "<>" is rejected with HTTP 422). urlEncode emits
    // it as %21.
    EXPECT_NE(path.find("%21"), std::string::npos);
    // Sort clause must be present in the URL.
    EXPECT_NE(path.find("sortBy="), std::string::npos);
}

TEST(OpTicketRepo, FindOpenInProjectBySubjectUsesContainsAndNotClosed) {
    Harness h;
    h.dispatcher.enqueueResponse(200, emptyCollection());

    (void)drainSync(h.tickets.findOpenInProjectBySubject(aid::ProjectId{"3"}, "Incognito Caller"));
    ASSERT_EQ(h.dispatcher.calls().size(), 1U);
    const auto& path = h.dispatcher.calls()[0].path;
    // Subject must use the "~" (contains) operator — OpenProject rejects
    // operator "=" on subject with HTTP 422. "~" is unreserved, stays literal.
    EXPECT_NE(path.find("subject"), std::string::npos);
    EXPECT_NE(path.find("~"), std::string::npos);
    // status!=Closed uses the "!" operator (encoded %21), never "<>" (%3C%3E).
    EXPECT_NE(path.find("%21"), std::string::npos);
    EXPECT_EQ(path.find("%3C%3E"), std::string::npos);
}

// ─── create ──────────────────────────────────────────────────────────────

TEST(OpTicketRepo, CreatePostsToProjectEndpointAndReturnsId) {
    Harness h;
    json resp;
    resp["id"] = 100;
    h.dispatcher.enqueueResponse(201, resp.dump());

    aid::NewTicket nt;
    nt.projectId = aid::ProjectId{"11"};
    nt.subject = "Call";
    nt.status = aid::TicketStatus::New;
    nt.callId = aid::CallId{"c-1"};
    nt.callerNumber = aid::PhoneNumber{"+491234"};

    auto r = drainSync(h.tickets.create(nt));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->v, "100");
    ASSERT_EQ(h.dispatcher.calls().size(), 1U);
    EXPECT_EQ(h.dispatcher.calls()[0].method, "POST");
    EXPECT_EQ(h.dispatcher.calls()[0].path, "/api/v3/projects/11/work_packages");
}

// ─── save + retryOn409 wiring ────────────────────────────────────────────
//
// save(id, reduce) fetches the ticket's fresh server state, applies the pure
// reducer to it, and PATCHes the result through retryOn409 — re-applying the
// reducer to the re-fetched state on every 409. So every
// save begins with a GET (the seed fetch) the old snapshot-taking save lacked.

namespace {
// The trivial "persist the fetched state as-is" reducer — for tests whose
// assertions are about the PATCH wiring, not a field mutation.
aid::Ticket identity(aid::Ticket t) {
    return t;
}
} // namespace

TEST(OpTicketRepo, SaveSucceedsFirstTryNoRetry) {
    Harness h;
    h.dispatcher.enqueueResponse(200, halTicket("42", 3, "/api/v3/statuses/2")); // seed fetch
    h.dispatcher.enqueueResponse(200, halTicket("42", 4, "/api/v3/statuses/2")); // PATCH

    auto r = drainSync(h.tickets.save(aid::TicketId{"42"}, identity));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->lockVersion, 4); // refreshed from PATCH response
    // GET (seed fetch) + PATCH — no conflict, no backoff.
    ASSERT_EQ(h.dispatcher.calls().size(), 2U);
    EXPECT_EQ(h.dispatcher.calls()[0].method, "GET");
    EXPECT_EQ(h.dispatcher.calls()[1].method, "PATCH");
    EXPECT_TRUE(h.sleeper.durations.empty());
}

TEST(OpTicketRepo, SaveRetriesOn409ThenSucceedsWithFreshLockVersion) {
    Harness h;
    // Seed fetch (lockVersion=3) → reducer applies → 1st PATCH → 409. The
    // 409-refresh GET returns lockVersion=10; the 2nd PATCH then succeeds.
    h.dispatcher.enqueueResponse(200, halTicket("42", 3, "/api/v3/statuses/2"));  // seed fetch
    h.dispatcher.enqueueResponse(409, "{}");                                      // 1st PATCH
    h.dispatcher.enqueueResponse(200, halTicket("42", 10, "/api/v3/statuses/2")); // refresh
    h.dispatcher.enqueueResponse(200, halTicket("42", 11, "/api/v3/statuses/2")); // 2nd PATCH

    auto r = drainSync(h.tickets.save(aid::TicketId{"42"}, identity));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->lockVersion, 11);
    // 4 HTTP calls: GET (seed), PATCH→409, GET (refresh), PATCH→200.
    ASSERT_EQ(h.dispatcher.calls().size(), 4U);
    EXPECT_EQ(h.dispatcher.calls()[0].method, "GET");
    EXPECT_EQ(h.dispatcher.calls()[1].method, "PATCH");
    EXPECT_EQ(h.dispatcher.calls()[2].method, "GET");
    EXPECT_EQ(h.dispatcher.calls()[3].method, "PATCH");
    // Exactly one backoff sleep at 50 ms (first attempt's backoff).
    ASSERT_EQ(h.sleeper.durations.size(), 1U);
    EXPECT_EQ(h.sleeper.durations[0].count(), 50);
}

// The retry re-derives the delta against the FRESH server state, not the stale
// first snapshot — the core scenario. The reducer appends a callid;
// between our seed fetch and our PATCH a sibling writer added its own callid AND
// closed a call-log line. The 409-refresh must hand the reducer that sibling
// state so BOTH edits survive in the retried PATCH body.
TEST(OpTicketRepo, SaveReDerivesDeltaAgainstFreshStateOn409) {
    Harness h;
    // Seed: callId="A", line (A) open.
    json seed = json::parse(halTicket("42", 3, "/api/v3/statuses/2"));
    seed["customField1"] = "A";
    seed["customField6"] = {{"format", "markdown"}, {"raw", "alice: Call start: 10:00 (A)"}};
    h.dispatcher.enqueueResponse(200, seed.dump());
    // Our 1st PATCH loses the race.
    h.dispatcher.enqueueResponse(409, "{}");
    // 409-refresh: a sibling Hangup landed — callId now "A,B" with B's line, and
    // it also stamped a description edit. (Our reducer must preserve all of it.)
    json fresh = json::parse(halTicket("42", 4, "/api/v3/statuses/2"));
    fresh["customField1"] = "A,B";
    fresh["customField6"] = {{"format", "markdown"},
                             {"raw", "alice: Call start: 10:00 (A)\nbob: Call start: 10:05 (B)"}};
    fresh["description"] = {{"format", "markdown"}, {"raw", "sibling comment"}};
    h.dispatcher.enqueueResponse(200, fresh.dump());
    h.dispatcher.enqueueResponse(200, halTicket("42", 5, "/api/v3/statuses/2")); // 2nd PATCH ok

    // Our delta: append callid "C" to whatever the fresh list holds.
    auto r = drainSync(h.tickets.save(aid::TicketId{"42"}, [](aid::Ticket t) {
        t.callIds.push_back(aid::CallId{"C"});
        return t;
    }));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    ASSERT_EQ(h.dispatcher.calls().size(), 4U);
    // The retried PATCH body must carry the sibling's "A,B" PLUS our "C", and the
    // sibling's call-log line + description — none of it clobbered by a stale
    // snapshot.
    const auto retry = json::parse(h.dispatcher.calls()[3].body);
    EXPECT_EQ(retry["lockVersion"], 4) << "retry uses the refreshed lockVersion";
    EXPECT_EQ(retry["customField1"], "A,B,C") << "our callid unions onto the sibling's fresh list";
    EXPECT_NE(retry["customField6"]["raw"].get<std::string>().find("(B)"), std::string::npos)
        << "sibling's call-log line survives the retry";
    EXPECT_EQ(retry["description"]["raw"], "sibling comment")
        << "sibling's description edit survives the retry";
}

// ─── save assignee resolution (numeric/href/display-name round-trip) ──────

// A ticket fetched WITHOUT ?include=assignee carries a display-name title (or
// numeric id) in `assignee`, NOT a login. hangup/closeTwoStep re-save it
// unchanged. A display name is shape-indistinguishable from a login, so save()
// does try hrefFor — but on the (inevitable) miss it OMITS the assignee link
// instead of aborting the whole event, leaving OpenProject's stored assignee
// intact. Regression for the "OpUserRepo::hrefFor: no users matched login <X>"
// hangup bug.
TEST(OpTicketRepo, SaveWithDisplayNameAssigneeOmitsLinkAndSucceeds) {
    Harness h;
    h.dispatcher.enqueueResponse(200, halTicket("42", 3, "/api/v3/statuses/2")); // seed fetch
    h.dispatcher.enqueueResponse(200, emptyCollection()); // users lookup: no match
    h.dispatcher.enqueueResponse(200, halTicket("42", 4, "/api/v3/statuses/2"));

    auto setAssignee = [](aid::Ticket t) {
        t.assignee = aid::UserHandle{"dia dia"}; // display name, not a login
        return t;
    };
    auto r = drainSync(h.tickets.save(aid::TicketId{"42"}, setAssignee));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    // The lookup miss does NOT abort the save; the PATCH still goes out, just
    // without an assignee link.
    ASSERT_EQ(h.dispatcher.calls().size(), 3U);
    EXPECT_EQ(h.dispatcher.calls()[0].method, "GET");   // seed fetch
    EXPECT_EQ(h.dispatcher.calls()[1].method, "GET");   // users lookup (misses)
    EXPECT_EQ(h.dispatcher.calls()[2].method, "PATCH"); // still saves
    const auto body = json::parse(h.dispatcher.calls()[2].body);
    EXPECT_FALSE(body["_links"].contains("assignee"));
}

TEST(OpTicketRepo, SaveWithNumericAssigneeBuildsHrefWithoutHttp) {
    Harness h;
    h.dispatcher.enqueueResponse(200, halTicket("42", 3, "/api/v3/statuses/2")); // seed fetch
    h.dispatcher.enqueueResponse(200, halTicket("42", 4, "/api/v3/statuses/2"));

    auto setAssignee = [](aid::Ticket t) {
        t.assignee = aid::UserHandle{"6"}; // bare numeric user id
        return t;
    };
    auto r = drainSync(h.tickets.save(aid::TicketId{"42"}, setAssignee));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    // Seed fetch + PATCH only — no users lookup (href rebuilt from the numeric id).
    ASSERT_EQ(h.dispatcher.calls().size(), 2U);
    EXPECT_EQ(h.dispatcher.calls()[0].method, "GET");
    EXPECT_EQ(h.dispatcher.calls()[1].method, "PATCH");
    const auto body = json::parse(h.dispatcher.calls()[1].body);
    EXPECT_EQ(body["_links"]["assignee"]["href"], "/api/v3/users/6");
}

TEST(OpTicketRepo, SaveWithHrefAssigneeUsesVerbatim) {
    Harness h;
    h.dispatcher.enqueueResponse(200, halTicket("42", 3, "/api/v3/statuses/2")); // seed fetch
    h.dispatcher.enqueueResponse(200, halTicket("42", 4, "/api/v3/statuses/2"));

    auto setAssignee = [](aid::Ticket t) {
        t.assignee = aid::UserHandle{"/api/v3/users/9"}; // already a full href
        return t;
    };
    auto r = drainSync(h.tickets.save(aid::TicketId{"42"}, setAssignee));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    ASSERT_EQ(h.dispatcher.calls().size(), 2U); // seed fetch + PATCH
    EXPECT_EQ(h.dispatcher.calls()[0].method, "GET");
    EXPECT_EQ(h.dispatcher.calls()[1].method, "PATCH");
    const auto body = json::parse(h.dispatcher.calls()[1].body);
    EXPECT_EQ(body["_links"]["assignee"]["href"], "/api/v3/users/9");
}

TEST(OpTicketRepo, SaveWithLoginAssigneeResolvesViaHrefFor) {
    Harness h;
    h.dispatcher.enqueueResponse(200, halTicket("42", 3, "/api/v3/statuses/2")); // seed fetch
    // Then a call resolves the login → href; finally the PATCH.
    json users;
    users["_embedded"]["elements"] = json::array();
    json one;
    one["login"] = "alice";
    one["_links"]["self"]["href"] = "/api/v3/users/9";
    users["_embedded"]["elements"].push_back(std::move(one));
    h.dispatcher.enqueueResponse(200, users.dump());
    h.dispatcher.enqueueResponse(200, halTicket("42", 4, "/api/v3/statuses/2"));

    auto setAssignee = [](aid::Ticket t) {
        t.assignee = aid::UserHandle{"alice"}; // login form
        return t;
    };
    auto r = drainSync(h.tickets.save(aid::TicketId{"42"}, setAssignee));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    ASSERT_EQ(h.dispatcher.calls().size(), 3U);
    EXPECT_EQ(h.dispatcher.calls()[0].method, "GET"); // seed fetch
    EXPECT_EQ(h.dispatcher.calls()[1].method, "GET"); // users lookup
    EXPECT_EQ(h.dispatcher.calls()[2].method, "PATCH");
    const auto body = json::parse(h.dispatcher.calls()[2].body);
    EXPECT_EQ(body["_links"]["assignee"]["href"], "/api/v3/users/9");
}

// ─── assignee 422-tolerance ──────────────────────────────────────────────
//
// OpenProject rejects assigning a work package to a non-member with HTTP 422.
// The callHandler CSV — not the single assignee — is the real visibility
// mechanism, so a 422 on the assignee must NOT fail the save: the adapter
// re-PATCHes once WITHOUT the assignee link so status/callLength still persist.

TEST(OpTicketRepo, SaveTolerates422OnAssigneeByReSavingWithoutIt) {
    Harness h;
    h.dispatcher.enqueueResponse(200, halTicket("42", 3, "/api/v3/statuses/2")); // seed fetch
    h.dispatcher.enqueueResponse(422, R"({"message":"assignee is not a valid member"})");
    h.dispatcher.enqueueResponse(200, halTicket("42", 4, "/api/v3/statuses/2")); // 2nd PATCH ok

    auto setAssignee = [](aid::Ticket t) {
        t.assignee = aid::UserHandle{"/api/v3/users/9"}; // not a project member
        return t;
    };
    auto r = drainSync(h.tickets.save(aid::TicketId{"42"}, setAssignee));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->lockVersion, 4);
    ASSERT_EQ(h.dispatcher.calls().size(), 3U);
    EXPECT_EQ(h.dispatcher.calls()[0].method, "GET"); // seed fetch
    EXPECT_EQ(h.dispatcher.calls()[1].method, "PATCH");
    EXPECT_TRUE(json::parse(h.dispatcher.calls()[1].body)["_links"].contains("assignee"))
        << "first attempt includes the assignee link";
    EXPECT_EQ(h.dispatcher.calls()[2].method, "PATCH");
    EXPECT_FALSE(json::parse(h.dispatcher.calls()[2].body)["_links"].contains("assignee"))
        << "retry drops the rejected assignee link";
    // The retry is NOT a 409 backoff — no sleep.
    EXPECT_TRUE(h.sleeper.durations.empty());
}

TEST(OpTicketRepo, Save422WithoutAssigneePropagates) {
    Harness h;
    h.dispatcher.enqueueResponse(200, halTicket("42", 3, "/api/v3/statuses/2")); // seed fetch
    // A 422 unrelated to the assignee (no assignee link to drop) is a real error.
    h.dispatcher.enqueueResponse(422, R"({"message":"subject can't be blank"})");

    // Reducer leaves the (absent) assignee alone.
    auto r = drainSync(h.tickets.save(aid::TicketId{"42"}, identity));
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ErrorCode::Unprocessable422);
    ASSERT_EQ(h.dispatcher.calls().size(), 2U) << "seed fetch + one PATCH; no assignee → no retry";
}

// ─── addCallHandler — refetch → union → minimal patch (merge-on-409) ──────

TEST(OpTicketRepo, AddCallHandlerAppendsLoginViaMinimalPatch) {
    Harness h;
    json wp = json::parse(halTicket("42", 5, "/api/v3/statuses/2"));
    wp["customField7"]["raw"] = "alice";                                         // existing handler
    h.dispatcher.enqueueResponse(200, wp.dump());                                // refetch
    h.dispatcher.enqueueResponse(200, halTicket("42", 6, "/api/v3/statuses/2")); // PATCH ok

    auto r = drainSync(h.tickets.addCallHandler(aid::TicketId{"42"}, aid::UserHandle{"bob"}));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    ASSERT_EQ(h.dispatcher.calls().size(), 2U);
    EXPECT_EQ(h.dispatcher.calls()[0].method, "GET");
    EXPECT_EQ(h.dispatcher.calls()[0].path, "/api/v3/work_packages/42");
    EXPECT_EQ(h.dispatcher.calls()[1].method, "PATCH");
    const auto body = json::parse(h.dispatcher.calls()[1].body);
    // Minimal body: lockVersion + callHandler ONLY (no status/subject/etc.).
    EXPECT_EQ(body["lockVersion"], 5);
    EXPECT_EQ(body["customField7"]["raw"], "alice, bob") << "unions onto the fresh CSV";
    EXPECT_FALSE(body.contains("_links"));
    EXPECT_FALSE(body.contains("subject"));
}

TEST(OpTicketRepo, AddCallHandlerNoOpWhenAlreadyPresent) {
    Harness h;
    json wp = json::parse(halTicket("42", 5, "/api/v3/statuses/2"));
    wp["customField7"]["raw"] = "alice, bob";
    h.dispatcher.enqueueResponse(200, wp.dump()); // refetch only

    auto r = drainSync(h.tickets.addCallHandler(aid::TicketId{"42"}, aid::UserHandle{"bob"}));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    // Already recorded → "append only if absent" means NO patch at all.
    ASSERT_EQ(h.dispatcher.calls().size(), 1U);
    EXPECT_EQ(h.dispatcher.calls()[0].method, "GET");
}

// Two racing accepts: between our refetch and our PATCH, another mailbox recorded
// its own handler and bumped lockVersion → 409. The retry re-reads the FRESH CSV
// (now carrying the other handler) and re-unions ours, so BOTH survive.
TEST(OpTicketRepo, AddCallHandlerReMergesOn409KeepingBothHandlers) {
    Harness h;
    json first = json::parse(halTicket("42", 5, "/api/v3/statuses/2"));
    first["customField7"]["raw"] = "alice"; // initial refetch
    h.dispatcher.enqueueResponse(200, first.dump());
    h.dispatcher.enqueueResponse(409, "{}"); // our PATCH loses the race
    json fresh = json::parse(halTicket("42", 6, "/api/v3/statuses/2"));
    fresh["customField7"]["raw"] = "alice, carol";   // a concurrent accept landed
    h.dispatcher.enqueueResponse(200, fresh.dump()); // 409-refresh
    h.dispatcher.enqueueResponse(200, halTicket("42", 7, "/api/v3/statuses/2")); // 2nd PATCH ok

    auto r = drainSync(h.tickets.addCallHandler(aid::TicketId{"42"}, aid::UserHandle{"bob"}));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    // GET, PATCH(409), GET(refresh), PATCH(ok) = 4 calls; one 50 ms backoff.
    ASSERT_EQ(h.dispatcher.calls().size(), 4U);
    EXPECT_EQ(h.dispatcher.calls()[1].method, "PATCH");
    EXPECT_EQ(h.dispatcher.calls()[3].method, "PATCH");
    const auto firstBody = json::parse(h.dispatcher.calls()[1].body);
    EXPECT_EQ(firstBody["customField7"]["raw"], "alice, bob");
    const auto retryBody = json::parse(h.dispatcher.calls()[3].body);
    EXPECT_EQ(retryBody["lockVersion"], 6) << "retry uses the refreshed lockVersion";
    EXPECT_EQ(retryBody["customField7"]["raw"], "alice, carol, bob")
        << "re-merge keeps the concurrently-recorded handler AND ours";
    ASSERT_EQ(h.sleeper.durations.size(), 1U);
    EXPECT_EQ(h.sleeper.durations[0].count(), 50);
}

// Phase 6 echo suppression: addCallHandler's own PATCH must record its
// post-PATCH version in the ledger, so the webhook OpenProject fires for this
// very edit is recognised as our echo and dropped instead of re-broadcast.
TEST(OpTicketRepo, AddCallHandlerRecordsPostPatchVersionForEchoSuppression) {
    Harness h;
    // GET (seed: handler absent at v3) → PATCH (response carries the new v4).
    h.dispatcher.enqueueResponse(200, halTicket("42", 3, "/api/v3/statuses/2"));
    h.dispatcher.enqueueResponse(200, halTicket("42", 4, "/api/v3/statuses/2"));

    auto r = drainSync(h.tickets.addCallHandler(aid::TicketId{"42"}, aid::UserHandle{"alice"}));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_TRUE(h.ledger.contains(aid::TicketId{"42"}, 4)) << "post-PATCH version recorded";
    EXPECT_FALSE(h.ledger.contains(aid::TicketId{"42"}, 3)) << "exact-version match only";
}

// create() keeps propagating a lookup failure — its callers pass a fresh login
// that is supposed to exist, so a miss is a real fault, not a no-op.
TEST(OpTicketRepo, CreateWithUnresolvableAssigneePropagatesError) {
    Harness h;
    h.dispatcher.enqueueResponse(200, emptyCollection()); // users lookup: no match

    aid::NewTicket nt;
    nt.projectId = aid::ProjectId{"11"};
    nt.subject = "Call";
    nt.status = aid::TicketStatus::New;
    nt.callId = aid::CallId{"c-1"};
    nt.callerNumber = aid::PhoneNumber{"+491234"};
    nt.assignee = aid::UserHandle{"ghost"};

    auto r = drainSync(h.tickets.create(nt));
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ErrorCode::NotFound);
    // No POST was issued — we aborted on the failed user lookup.
    ASSERT_EQ(h.dispatcher.calls().size(), 1U);
    EXPECT_EQ(h.dispatcher.calls()[0].method, "GET");
}

// ─── closeTwoStep ─────────────────────────────────────────────────────────

TEST(OpTicketRepo, CloseTwoStepFromClosedIsIdempotentZeroPatches) {
    Harness h;
    // fetchById returns a closed ticket. path(Closed, Closed) = [] →
    // closeTwoStep makes no PATCHes.
    h.dispatcher.enqueueResponse(200, halTicket("42", 5, "/api/v3/statuses/3"));

    auto r = drainSync(h.tickets.closeTwoStep(aid::TicketId{"42"}));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    ASSERT_EQ(h.dispatcher.calls().size(), 1U); // only the fetchById GET
    EXPECT_EQ(h.dispatcher.calls()[0].method, "GET");
}

TEST(OpTicketRepo, CloseTwoStepFromInProgressIssuesOnePatchToClosed) {
    Harness h;
    // closeTwoStep first GETs to read the status (→ path). Then the single
    // Closed step is a save(id, reducer), which itself GETs the fresh state
    // before PATCHing — so the per-step write is now GET + PATCH.
    h.dispatcher.enqueueResponse(200, halTicket("42", 5, "/api/v3/statuses/2")); // path GET: InProg
    h.dispatcher.enqueueResponse(200, halTicket("42", 5, "/api/v3/statuses/2")); // save seed fetch
    h.dispatcher.enqueueResponse(200, halTicket("42", 6, "/api/v3/statuses/3")); // PATCH → Closed

    auto r = drainSync(h.tickets.closeTwoStep(aid::TicketId{"42"}));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    ASSERT_EQ(h.dispatcher.calls().size(), 3U);
    EXPECT_EQ(h.dispatcher.calls()[0].method, "GET");
    EXPECT_EQ(h.dispatcher.calls()[1].method, "GET");
    EXPECT_EQ(h.dispatcher.calls()[2].method, "PATCH");
    // The PATCH body must point at /api/v3/statuses/3 (Closed per cfg).
    EXPECT_NE(h.dispatcher.calls()[2].body.find("/api/v3/statuses/3"), std::string::npos);
}

TEST(OpTicketRepo, CloseTwoStepFromNewWalksThroughInProgressThenClosed) {
    Harness h;
    // path GET (New) → steps [InProgress, Closed]. Each step is GET (seed) +
    // PATCH; the seed for the 2nd step reflects the 1st step's write.
    h.dispatcher.enqueueResponse(200, halTicket("42", 5, "/api/v3/statuses/1")); // path GET: New
    h.dispatcher.enqueueResponse(200, halTicket("42", 5, "/api/v3/statuses/1")); // step1 seed: New
    h.dispatcher.enqueueResponse(200,
                                 halTicket("42", 6, "/api/v3/statuses/2")); // PATCH → InProgress
    h.dispatcher.enqueueResponse(
        200, halTicket("42", 6, "/api/v3/statuses/2")); // step2 seed: InProgress
    h.dispatcher.enqueueResponse(200, halTicket("42", 7, "/api/v3/statuses/3")); // PATCH → Closed

    auto r = drainSync(h.tickets.closeTwoStep(aid::TicketId{"42"}));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    ASSERT_EQ(h.dispatcher.calls().size(), 5U);
    EXPECT_EQ(h.dispatcher.calls()[0].method, "GET");   // path
    EXPECT_EQ(h.dispatcher.calls()[1].method, "GET");   // step1 seed
    EXPECT_EQ(h.dispatcher.calls()[2].method, "PATCH"); // → InProgress
    EXPECT_EQ(h.dispatcher.calls()[3].method, "GET");   // step2 seed
    EXPECT_EQ(h.dispatcher.calls()[4].method, "PATCH"); // → Closed
    EXPECT_NE(h.dispatcher.calls()[2].body.find("/api/v3/statuses/2"), std::string::npos);
    EXPECT_NE(h.dispatcher.calls()[4].body.find("/api/v3/statuses/3"), std::string::npos);
}

TEST(OpTicketRepo, CloseTwoStepGivesUpAfterFiveConsecutive409sPerStep) {
    Harness h;
    // path GET → InProgress so closeTwoStep needs one (Closed) step.
    h.dispatcher.enqueueResponse(200, halTicket("42", 5, "/api/v3/statuses/2")); // path GET
    // The save's seed fetch, then each of the 5 retry attempts: PATCH→409, GET
    // (refresh) returns InProgress with bumped lockVersion. After 5 conflicts in
    // a row the adapter must return LockVersionExhausted.
    h.dispatcher.enqueueResponse(200, halTicket("42", 5, "/api/v3/statuses/2")); // save seed fetch
    for (int i = 0; i < 5; ++i) {
        h.dispatcher.enqueueResponse(409, "{}");
        h.dispatcher.enqueueResponse(200, halTicket("42", 6 + i, "/api/v3/statuses/2")); // refresh
    }

    auto r = drainSync(h.tickets.closeTwoStep(aid::TicketId{"42"}));
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ErrorCode::LockVersionExhausted);
    // 1 path GET + 1 save seed GET + 5 (PATCH+GET) = 12 calls; 5 backoff sleeps.
    EXPECT_EQ(h.dispatcher.calls().size(), 12U);
    ASSERT_EQ(h.sleeper.durations.size(), 5U);
    EXPECT_EQ(h.sleeper.durations[0].count(), 50);
    EXPECT_EQ(h.sleeper.durations[4].count(), 800);
}

// CloseTicket + a concurrent Hangup/comment. Because each
// status step now re-fetches the ticket and sets ONLY status on that fresh
// state, an edit that landed BETWEEN the two status PATCHes is carried by the
// later PATCH instead of being clobbered by a stale snapshot. Here a sibling
// Hangup closes a call-log line after the New→InProgress PATCH but before the
// InProgress→Closed PATCH; the Closed PATCH body must carry the sibling's edit.
TEST(OpTicketRepo, CloseTwoStepPreservesConcurrentCallLogEditAcrossSteps) {
    Harness h;
    // path GET: New, with an OPEN call-log line.
    json pathGet = json::parse(halTicket("42", 5, "/api/v3/statuses/1"));
    pathGet["customField6"] = {{"format", "markdown"}, {"raw", "alice: Call start: 10:00 (A)"}};
    h.dispatcher.enqueueResponse(200, pathGet.dump());
    // step1 (→InProgress): seed still shows the open line; PATCH succeeds.
    h.dispatcher.enqueueResponse(200, pathGet.dump());
    h.dispatcher.enqueueResponse(200, halTicket("42", 6, "/api/v3/statuses/2"));
    // step2 (→Closed): seed now reflects a CONCURRENT hangup that closed the line.
    json afterHangup = json::parse(halTicket("42", 6, "/api/v3/statuses/2"));
    afterHangup["customField6"] = {{"format", "markdown"},
                                   {"raw", "alice: Call start: 10:00 Call End: 10:09"}};
    h.dispatcher.enqueueResponse(200, afterHangup.dump());
    h.dispatcher.enqueueResponse(200, halTicket("42", 7, "/api/v3/statuses/3"));

    auto r = drainSync(h.tickets.closeTwoStep(aid::TicketId{"42"}));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    ASSERT_EQ(h.dispatcher.calls().size(), 5U);
    // The Closed PATCH (last call) must carry the sibling hangup's edit, not the
    // stale open line from the original snapshot.
    const auto closedBody = json::parse(h.dispatcher.calls()[4].body);
    EXPECT_NE(closedBody["customField6"]["raw"].get<std::string>().find("Call End:"),
              std::string::npos)
        << "the concurrent hangup's edit survives the close's second PATCH";
    EXPECT_NE(closedBody["_links"]["status"]["href"].get<std::string>().find("/api/v3/statuses/3"),
              std::string::npos);
}

// ─── findCallTicketsWithHandler (cross-project visibility arm) ─────────────

namespace {

// A HAL work_package carrying a callHandler (customField7) CSV — the
// {format, raw} Formattable shape parseFromHal expects.
std::string halTicketWithHandlers(const std::string& id, const std::string& statusHref,
                                  const std::string& handlerCsv) {
    json j = json::parse(halTicket(id, 1, statusHref));
    j["customField7"] = {{"format", "markdown"}, {"raw", handlerCsv}};
    return j.dump();
}

} // namespace

TEST(OpTicketRepo, FindCallTicketsWithHandlerFiltersTypeStatusAndHandler) {
    Harness h;
    h.dispatcher.enqueueResponse(
        200, halCollectionOf(halTicketWithHandlers("42", "/api/v3/statuses/1", "alice")));

    auto r = drainSync(h.tickets.findCallTicketsWithHandler(aid::UserHandle{"alice"}));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    ASSERT_EQ(r->size(), 1U);
    EXPECT_EQ((*r)[0].id.v, "42");

    ASSERT_EQ(h.dispatcher.calls().size(), 1U);
    const auto& path = h.dispatcher.calls()[0].path;
    EXPECT_NE(path.find("/api/v3/work_packages?filters="), std::string::npos);
    // customField7 (the callHandler field, id 7 in sampleFields) is the filter
    // field; the substring operator "~" (%7E) is used, not "=".
    EXPECT_NE(path.find("customField7"), std::string::npos);
    // "~" url-encodes to itself (unreserved); the substring `~%22` shows the
    // contains operator inside the JSON (operator value followed by a closing
    // quote). "=" would encode as %3D, so this distinguishes the two.
    EXPECT_NE(path.find("~%22"), std::string::npos) << "callHandler filter must use operator \"~\"";
    // type + status clauses ride along (same shape as the in-projects arm).
    EXPECT_NE(path.find("type"), std::string::npos);
    EXPECT_NE(path.find("status"), std::string::npos);
}

TEST(OpTicketRepo, FindCallTicketsWithHandlerDropsSubstringFalsePositive) {
    Harness h;
    // Server-side "~" over-matches: viewer "alice" is a substring of "malice",
    // so OpenProject returns this ticket — but alice is NOT an exact handler.
    h.dispatcher.enqueueResponse(
        200, halCollectionOf(halTicketWithHandlers("42", "/api/v3/statuses/1", "malice, bob")));

    auto r = drainSync(h.tickets.findCallTicketsWithHandler(aid::UserHandle{"alice"}));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_TRUE(r->empty()) << "substring-only match must be filtered to exact CSV membership";
}

TEST(OpTicketRepo, FindCallTicketsWithHandlerKeepsExactCsvMember) {
    Harness h;
    h.dispatcher.enqueueResponse(
        200, halCollectionOf(halTicketWithHandlers("42", "/api/v3/statuses/1", "bob, alice")));

    auto r = drainSync(h.tickets.findCallTicketsWithHandler(aid::UserHandle{"alice"}));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    ASSERT_EQ(r->size(), 1U);
    EXPECT_EQ((*r)[0].id.v, "42");
}

// ─── pagination (dashboard snapshot arms loop until exhausted) ─────────────

namespace {

// A HAL collection of `n` minimal tickets with ascending ids starting at
// `firstId`, plus an optional top-level `total`. Used to script the
// multi-page responses a real OpenProject sends when a filter matches more
// than one page of work packages.
std::string halCollectionN(int firstId, int n, long long total = -1) {
    json env;
    env["_embedded"]["elements"] = json::array();
    for (int i = 0; i < n; ++i)
        env["_embedded"]["elements"].push_back(
            json::parse(halTicket(std::to_string(firstId + i), 1, "/api/v3/statuses/1")));
    if (total >= 0)
        env["total"] = total;
    return env.dump();
}

} // namespace

TEST(OpTicketRepo, FindCallTicketsInProjectsOpenPagesUntilExhausted) {
    Harness h;
    // Page 1 is a FULL page (kDashboardPageSize=200), so the loop cannot assume
    // it is the last and must fetch page 2. Page 2 is short (3 rows) → stop.
    // Without pagination OpenProject would cap this at its default 20 and the
    // dashboard would silently lose 183 tickets.
    h.dispatcher.enqueueResponse(200, halCollectionN(1, 200, 203));
    h.dispatcher.enqueueResponse(200, halCollectionN(201, 3, 203));

    auto r = drainSync(h.tickets.findCallTicketsInProjectsOpen({aid::ProjectId{"11"}}));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->size(), 203U) << "every ticket across both pages must be returned";
    EXPECT_EQ((*r)[0].id.v, "1");
    EXPECT_EQ((*r)[202].id.v, "203");

    ASSERT_EQ(h.dispatcher.calls().size(), 2U) << "total=203 not yet collected after page 1";
    EXPECT_NE(h.dispatcher.calls()[0].path.find("offset=1"), std::string::npos);
    EXPECT_NE(h.dispatcher.calls()[0].path.find("pageSize=1000"), std::string::npos);
    EXPECT_NE(h.dispatcher.calls()[1].path.find("offset=2"), std::string::npos);
}

TEST(OpTicketRepo, GetAllPagedHonoursTotalEvenWhenServerClampsPageSize) {
    Harness h;
    // Simulate OpenProject clamping our requested pageSize=1000 down to its
    // server max (here 100): every full page returns 100 elements — FEWER than
    // we asked for. A "stop when the page is shorter than requested" rule would
    // quit after page 1 and silently lose 150 tickets. Total-driven termination
    // must instead keep paging (offset 1,2,3) until all `total`=250 are in hand.
    auto clampedPage = [](int firstId, int n, long long total) {
        json env;
        env["_embedded"]["elements"] = json::array();
        for (int i = 0; i < n; ++i)
            env["_embedded"]["elements"].push_back(
                json::parse(halTicket(std::to_string(firstId + i), 1, "/api/v3/statuses/1")));
        env["total"] = total;
        env["pageSize"] = 100; // what the server actually applied (the clamp)
        return env.dump();
    };
    h.dispatcher.enqueueResponse(200, clampedPage(1, 100, 250));
    h.dispatcher.enqueueResponse(200, clampedPage(101, 100, 250));
    h.dispatcher.enqueueResponse(200, clampedPage(201, 50, 250));

    auto r = drainSync(h.tickets.findCallTicketsInProjectsOpen({aid::ProjectId{"11"}}));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->size(), 250U) << "must page by total, not by the requested pageSize";
    ASSERT_EQ(h.dispatcher.calls().size(), 3U) << "a clamped page is NOT mistaken for the last one";
    EXPECT_NE(h.dispatcher.calls()[0].path.find("pageSize=1000"), std::string::npos);
    EXPECT_NE(h.dispatcher.calls()[2].path.find("offset=3"), std::string::npos);
}

TEST(OpTicketRepo, FindCallTicketsInProjectsOpenSinglePageIssuesOneGet) {
    Harness h;
    // A result set smaller than one page must cost exactly ONE GET — the live-
    // delta query budget depends on the snapshot arms never over-fetching.
    h.dispatcher.enqueueResponse(200, halCollectionOf(halTicket("42", 1, "/api/v3/statuses/1")));

    auto r = drainSync(h.tickets.findCallTicketsInProjectsOpen({aid::ProjectId{"11"}}));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    ASSERT_EQ(r->size(), 1U);
    EXPECT_EQ(h.dispatcher.calls().size(), 1U) << "short page → no needless second GET";
}

TEST(OpTicketRepo, FindCallTicketsWithHandlerPagesThenPostFiltersAcrossPages) {
    Harness h;
    // The exact-CSV post-filter must run over EVERY page, not just the first.
    // Page 1 (full) holds a substring false positive ("malice"); page 2 (short)
    // holds the real exact handler. Pagination + post-filter together yield the
    // one real hit and drop the over-match.
    json full;
    full["_embedded"]["elements"] = json::array();
    full["_embedded"]["elements"].push_back(
        json::parse(halTicketWithHandlers("1", "/api/v3/statuses/1", "malice")));
    for (int i = 2; i <= 200; ++i)
        full["_embedded"]["elements"].push_back(
            json::parse(halTicketWithHandlers(std::to_string(i), "/api/v3/statuses/1", "bob")));
    full["total"] = 201;
    h.dispatcher.enqueueResponse(200, full.dump());
    h.dispatcher.enqueueResponse(
        200, halCollectionOf(halTicketWithHandlers("201", "/api/v3/statuses/1", "alice, bob")));

    auto r = drainSync(h.tickets.findCallTicketsWithHandler(aid::UserHandle{"alice"}));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    ASSERT_EQ(r->size(), 1U) << "only the page-2 exact match survives";
    EXPECT_EQ((*r)[0].id.v, "201");
    EXPECT_EQ(h.dispatcher.calls().size(), 2U);
}

// ─── openCallsInProject (per-project membership-reconciler listing) ────────

TEST(OpTicketRepo, OpenCallsInProjectPagesUntilExhausted) {
    Harness h;
    // Same pagination contract as the dashboard arms: a busy project past
    // OpenProject's default 20-row page must not be silently truncated. Page 1
    // is full (kDashboardPageSize=1000 requested → fake returns 200), total=203
    // not yet reached → page 2 (short) completes the set.
    h.dispatcher.enqueueResponse(200, halCollectionN(1, 200, 203));
    h.dispatcher.enqueueResponse(200, halCollectionN(201, 3, 203));

    auto r = drainSync(h.tickets.openCallsInProject(aid::ProjectId{"11"}));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->size(), 203U) << "every open call ticket across both pages must be returned";
    EXPECT_EQ((*r)[0].id.v, "1");
    EXPECT_EQ((*r)[202].id.v, "203");

    ASSERT_EQ(h.dispatcher.calls().size(), 2U) << "total=203 not yet collected after page 1";
    EXPECT_NE(h.dispatcher.calls()[0].path.find("offset=1"), std::string::npos);
    EXPECT_NE(h.dispatcher.calls()[0].path.find("pageSize=1000"), std::string::npos);
    EXPECT_NE(h.dispatcher.calls()[1].path.find("offset=2"), std::string::npos);
}

TEST(OpTicketRepo, OpenCallsInProjectWithNoOpenCallsReturnsEmpty) {
    Harness h;
    h.dispatcher.enqueueResponse(200, emptyCollection());

    auto r = drainSync(h.tickets.openCallsInProject(aid::ProjectId{"11"}));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_TRUE(r->empty());
    EXPECT_EQ(h.dispatcher.calls().size(), 1U) << "empty page → exactly one GET";
}

TEST(OpTicketRepo, OpenCallsInProjectFiltersToProjectTypeAndOpenStatusesOnly) {
    Harness h;
    h.dispatcher.enqueueResponse(200, emptyCollection());

    auto r = drainSync(h.tickets.openCallsInProject(aid::ProjectId{"11"}));
    ASSERT_TRUE(r.has_value()) << r.error().message;

    ASSERT_EQ(h.dispatcher.calls().size(), 1U);
    const auto& path = h.dispatcher.calls()[0].path;
    // project + type=Call clauses present (field names ride through urlEncode
    // verbatim — only the surrounding quotes become %22).
    EXPECT_NE(path.find("project"), std::string::npos);
    EXPECT_NE(path.find("type"), std::string::npos);
    EXPECT_NE(path.find("status"), std::string::npos);
    // status in {New, InProgress}: the quoted status-id values "1" and "2"
    // (sampleCfg) appear in the encoded filter as %221%22 / %222%22.
    EXPECT_NE(path.find("%221%22"), std::string::npos) << "New status must be queried";
    EXPECT_NE(path.find("%222%22"), std::string::npos) << "InProgress status must be queried";
    // Closed (3) must be EXCLUDED — the query, not a post-filter, is what drops
    // it. No other clause emits this quoted id (project=11, type=7), so its
    // absence is unambiguous.
    EXPECT_EQ(path.find("%223%22"), std::string::npos) << "Closed must be excluded";
}

// ─── recipientsFor (members ∪ callHandlers) ───────────────────────────────

TEST(OpTicketRepo, RecipientsForUnionsProjectMembersAndCallHandlers) {
    Harness h;
    // projectMembers(11): the memberships collection names two user principals
    // by href; each is resolved to its login in a follow-up GET.
    json memberships;
    memberships["_embedded"]["elements"] = json::array();
    for (const char* href : {"/api/v3/users/1", "/api/v3/users/2"}) {
        json one;
        one["_links"]["principal"]["href"] = href;
        memberships["_embedded"]["elements"].push_back(std::move(one));
    }
    h.dispatcher.enqueueResponse(200, memberships.dump());
    const auto userJson = [](const char* login, int id) {
        json u;
        u["login"] = login;
        u["id"] = id;
        u["_links"]["self"]["href"] = "/api/v3/users/" + std::to_string(id);
        return u.dump();
    };
    h.dispatcher.enqueueResponse(200, userJson("alice", 1));
    h.dispatcher.enqueueResponse(200, userJson("bob", 2));

    aid::Ticket t;
    t.id = aid::TicketId{"42"};
    t.projectId = aid::ProjectId{"11"};
    // bob is both a member and a handler (must dedup); carol is handler-only.
    t.callHandlers = {aid::UserHandle{"bob"}, aid::UserHandle{"carol"}};

    auto r = drainSync(h.tickets.recipientsFor(t));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    ASSERT_EQ(r->size(), 3U) << "alice, bob, carol — bob deduped across the two arms";
    EXPECT_EQ((*r)[0].v, "alice");
    EXPECT_EQ((*r)[1].v, "bob");
    EXPECT_EQ((*r)[2].v, "carol");

    ASSERT_EQ(h.dispatcher.calls().size(), 3U);
    EXPECT_NE(h.dispatcher.calls()[0].path.find("/api/v3/memberships?filters="), std::string::npos);
    EXPECT_EQ(h.dispatcher.calls()[0].path.find("/projects/"), std::string::npos);
}

TEST(OpTicketRepo, RecipientsForWithNoMembersReturnsJustHandlers) {
    Harness h;
    h.dispatcher.enqueueResponse(200, emptyCollection());

    aid::Ticket t;
    t.id = aid::TicketId{"42"};
    t.projectId = aid::ProjectId{"99"};
    t.callHandlers = {aid::UserHandle{"carol"}};

    auto r = drainSync(h.tickets.recipientsFor(t));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    ASSERT_EQ(r->size(), 1U);
    EXPECT_EQ((*r)[0].v, "carol");
}

// ─── droppedRecipientsOnWebhook (S7 — admin removed a callHandler) ──────────

namespace {

// A memberships collection naming the given user-principal hrefs (the shape
// projectMembers GETs first); each principal is then resolved to its login.
std::string membershipsOf(const std::vector<std::string>& principalHrefs) {
    json j;
    j["_embedded"]["elements"] = json::array();
    for (const auto& href : principalHrefs) {
        json one;
        one["_links"]["principal"]["href"] = href;
        j["_embedded"]["elements"].push_back(std::move(one));
    }
    return j.dump();
}

std::string userResourceJson(const std::string& login, int id) {
    json u;
    u["login"] = login;
    u["id"] = id;
    u["_links"]["self"]["href"] = "/api/v3/users/" + std::to_string(id);
    return u.dump();
}

aid::Ticket ticketWithHandlers(const std::vector<std::string>& logins) {
    aid::Ticket t;
    t.id = aid::TicketId{"42"};
    t.projectId = aid::ProjectId{"11"};
    for (const auto& l : logins)
        t.callHandlers.push_back(aid::UserHandle{l});
    return t;
}

} // namespace

// A handler the admin removed who is NOT a project member loses visibility —
// surfaced so the webhook can push them a ticket_remove.
TEST(OpTicketRepo, DroppedHandlerNonMemberIsSurfaced) {
    Harness h;
    // Baseline: the daemon last saw {alice, bob} on this ticket.
    h.handlerLedger.record(aid::TicketId{"42"}, {aid::UserHandle{"alice"}, aid::UserHandle{"bob"}});
    // Project 11 has only alice as a member — bob is handler-only.
    h.dispatcher.enqueueResponse(200, membershipsOf({"/api/v3/users/1"}));
    h.dispatcher.enqueueResponse(200, userResourceJson("alice", 1));

    // Webhook ticket now carries only alice — bob was dropped from customField7.
    auto r = drainSync(h.tickets.droppedRecipientsOnWebhook(ticketWithHandlers({"alice"})));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    ASSERT_EQ(r->size(), 1U) << "bob was a handler, is no longer, and is not a member";
    EXPECT_EQ((*r)[0].v, "bob");
}

// A handler the admin removed who is STILL a project member keeps the ticket via
// the membership arm — must NOT be surfaced (no spurious ticket_remove).
TEST(OpTicketRepo, DroppedHandlerStillMemberIsNotSurfaced) {
    Harness h;
    h.handlerLedger.record(aid::TicketId{"42"}, {aid::UserHandle{"alice"}, aid::UserHandle{"bob"}});
    // Project 11 has both alice and bob as members.
    h.dispatcher.enqueueResponse(200, membershipsOf({"/api/v3/users/1", "/api/v3/users/2"}));
    h.dispatcher.enqueueResponse(200, userResourceJson("alice", 1));
    h.dispatcher.enqueueResponse(200, userResourceJson("bob", 2));

    auto r = drainSync(h.tickets.droppedRecipientsOnWebhook(ticketWithHandlers({"alice"})));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_TRUE(r->empty()) << "bob is still a member, so the membership arm keeps him";
}

// Cold start: no prior handler set is known for the ticket (e.g. first webhook
// after a restart). Nothing is surfaced and — crucially — no membership query is
// made, since there is no diff to evaluate.
TEST(OpTicketRepo, ColdStartSurfacesNothingAndMakesNoQuery) {
    Harness h;
    // No handlerLedger.record() — the ledger has never seen ticket 42.

    auto r = drainSync(h.tickets.droppedRecipientsOnWebhook(ticketWithHandlers({"alice"})));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_TRUE(r->empty()) << "no baseline to diff against ⇒ nothing dropped";
    EXPECT_EQ(h.dispatcher.calls().size(), 0U) << "cold start must not query membership";
}

// Symmetric to the cold-start test: a dashboard find-arm warms the handler-drop
// baseline (recordIfAbsent), so a subsequent admin drop is surfaced WITHOUT a
// prior fetch/save or seed webhook — the cold-start blind spot the find-arm
// warming fixes. The member arm (findCallTicketsInProjectsOpen) returns the
// ticket with {alice, bob}; clearing bob (a non-member) then surfaces bob.
TEST(OpTicketRepo, FindArmWarmsHandlerBaselineSoLaterDropIsSurfaced) {
    Harness h;
    // Dashboard load: the member arm returns ticket 42 carrying {alice, bob}.
    h.dispatcher.enqueueResponse(
        200, halCollectionOf(halTicketWithHandlers("42", "/api/v3/statuses/1", "alice, bob")));
    auto warm = drainSync(h.tickets.findCallTicketsInProjectsOpen({aid::ProjectId{"11"}}));
    ASSERT_TRUE(warm.has_value()) << warm.error().message;
    ASSERT_EQ(warm->size(), 1U);

    // Now the admin drops bob from customField7. Project 11 has only alice as a
    // member, so the webhook must surface bob (handler-only, now removed).
    h.dispatcher.enqueueResponse(200, membershipsOf({"/api/v3/users/1"}));
    h.dispatcher.enqueueResponse(200, userResourceJson("alice", 1));

    auto r = drainSync(h.tickets.droppedRecipientsOnWebhook(ticketWithHandlers({"alice"})));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    ASSERT_EQ(r->size(), 1U)
        << "the find-arm seeded {alice, bob}; bob was dropped and is no member";
    EXPECT_EQ((*r)[0].v, "bob");
}

// A failed membership fetch must NOT swallow the drop. exchange() has already
// consumed the prior baseline, so a naive error-return would diff the new set
// against itself next time and lose the drop forever. The error path restores the
// prior baseline (record), so a second webhook for the same (still-dropped) ticket
// re-surfaces the dropped non-member once membership resolves.
TEST(OpTicketRepo, MembershipFetchFailureRestoresBaselineForRetry) {
    Harness h;
    // Baseline: the daemon last saw {alice, bob} on this ticket.
    h.handlerLedger.record(aid::TicketId{"42"}, {aid::UserHandle{"alice"}, aid::UserHandle{"bob"}});

    // First webhook (bob dropped): the membership fetch fails transiently.
    h.dispatcher.enqueueError(ErrorCode::UpstreamUnavailable, "OP 503");
    auto first = drainSync(h.tickets.droppedRecipientsOnWebhook(ticketWithHandlers({"alice"})));
    ASSERT_FALSE(first.has_value()) << "the membership fetch failed ⇒ the round errors";
    EXPECT_EQ(first.error().code, ErrorCode::UpstreamUnavailable);

    // Second webhook for the same still-dropped ticket: membership now resolves.
    // Because the prior baseline {alice, bob} was restored on the error path, this
    // re-diffs to {bob} and surfaces him — proving the drop was not lost.
    h.dispatcher.enqueueResponse(200, membershipsOf({"/api/v3/users/1"}));
    h.dispatcher.enqueueResponse(200, userResourceJson("alice", 1));
    auto second = drainSync(h.tickets.droppedRecipientsOnWebhook(ticketWithHandlers({"alice"})));
    ASSERT_TRUE(second.has_value()) << second.error().message;
    ASSERT_EQ(second->size(), 1U) << "restored baseline ⇒ the drop retries and surfaces bob";
    EXPECT_EQ((*second)[0].v, "bob");
}
