#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "FakeAddressBook.h"
#include "FakeClock.h"
#include "FakeTicketStore.h"
#include "FakeUiNotifier.h"
#include "aid/crosscutting/Config.h"
#include "aid/plumbing/Error.h"
#include "aid/plumbing/Result.h"
#include "aid/usecases/HandleOutgoingCall.h"
#include "aid/value-types/CallEvent.h"
#include "aid/value-types/Contact.h"
#include "aid/value-types/Ids.h"
#include "aid/value-types/Ticket.h"

namespace {

using aid::CallId;
using aid::Contact;
using aid::OutgoingCall;
using aid::PhoneNumber;
using aid::ProjectId;
using aid::Ticket;
using aid::TicketId;
using aid::TicketStatus;
using aid::UserHandle;
using aid::crosscutting::Config;
using aid::fakes::FakeAddressBook;
using aid::fakes::FakeClock;
using aid::fakes::FakeTicketStore;
using aid::fakes::FakeUiNotifier;
using aid::plumbing::Error;
using aid::plumbing::ErrorCode;
using aid::plumbing::Result;
using aid::usecases::HandleOutgoingCall;

Ticket makeTicket(TicketId id, ProjectId pid, std::string subject = {},
                  std::vector<CallId> callIds = {}) {
    Ticket t;
    t.id = std::move(id);
    t.projectId = std::move(pid);
    t.subject = std::move(subject);
    t.status = TicketStatus::New;
    t.callIds = std::move(callIds);
    return t;
}

Result<void> sync(aid::plumbing::Task<Result<void>> task) {
    std::optional<Result<void>> sink;
    auto pump = [&]() -> aid::plumbing::Task<Result<void>> {
        auto r = co_await std::move(task);
        sink = std::move(r);
        co_return Result<void>{};
    };
    auto p = pump();
    EXPECT_TRUE(p.done());
    return std::move(*sink);
}

class HandleOutgoingCallTest : public ::testing::Test {
protected:
    FakeTicketStore ts_;
    FakeAddressBook ab_;
    FakeUiNotifier ui_;
    FakeClock clock_;
    Config::TicketRouting cfg_{ProjectId{"FB"}, "Incognito Caller"};

    HandleOutgoingCall makeUseCase() { return HandleOutgoingCall{ts_, ab_, ui_, clock_, cfg_}; }

    static OutgoingCall ev() {
        return OutgoingCall{CallId{"call-1"}, PhoneNumber{"+491701234567"}, UserHandle{"alice"}};
    }
};

TEST_F(HandleOutgoingCallTest, Known_WithOpenTicket_ReusesAndSetsAssignee) {
    ts_.nextResolveUser.push_back(std::optional<UserHandle>{UserHandle{"alice"}});
    ab_.canonicalizeMap["+491701234567"] = "+491701234567";
    Contact c;
    c.name = "Bob";
    c.projectIds = {ProjectId{"P1"}};
    ab_.nextLookup.push_back(std::optional<Contact>{c});

    auto open = makeTicket(TicketId{"T-open"}, ProjectId{"P1"}, "Bob");
    ts_.nextFindOpenInProjectByCallerNumber.push_back(std::optional<Ticket>{open});
    ts_.nextSave.push_back(open);
    ts_.nextAddCallHandler.push_back(Result<void>{});
    // After save + addCallHandler the use case re-fetches by id and emits.
    Ticket fresh;
    fresh.id = TicketId{"T-open"};
    fresh.status = TicketStatus::InProgress;
    ts_.nextFetchById.push_back(Result<Ticket>{fresh});
    ts_.nextRecipientsFor.push_back(
        Result<std::vector<UserHandle>>{std::vector<UserHandle>{UserHandle{"alice"}}});

    auto uc = makeUseCase();
    auto r = sync(uc.run(ev()));

    ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().message);
    ASSERT_EQ(ts_.saved.size(), 1U);
    EXPECT_EQ(ts_.saved[0].id, TicketId{"T-open"});
    ASSERT_TRUE(ts_.saved[0].assignee.has_value());
    EXPECT_EQ(ts_.saved[0].assignee->v, "alice"); // reused ticket had no assignee → set
    EXPECT_EQ(ts_.saved[0].status, TicketStatus::InProgress)
        << "outgoing call actively engages the reused (New) ticket → InProgress";
    ASSERT_EQ(ts_.addCallHandler_args.size(), 1U);
    EXPECT_EQ(ts_.addCallHandler_args[0].first, TicketId{"T-open"});
    EXPECT_EQ(ts_.addCallHandler_args[0].second.v, "alice");
    ASSERT_EQ(ts_.saved[0].callIds.size(), 1U);
    EXPECT_EQ(ts_.saved[0].callIds[0], CallId{"call-1"});
    EXPECT_TRUE(ts_.created.empty());
    // Live delta: re-fetched open ticket upserts to each recipient.
    EXPECT_TRUE(ui_.ticketRemoves.empty());
    ASSERT_EQ(ui_.ticketUpserts.size(), 1U);
    EXPECT_EQ(ui_.ticketUpserts[0].first, UserHandle{"alice"});
    EXPECT_EQ(ui_.ticketUpserts[0].second.id, TicketId{"T-open"});
}

TEST_F(HandleOutgoingCallTest, Known_NoOpenTicket_CreatesWithAssigneeAndNoCalledNumber) {
    ts_.nextResolveUser.push_back(std::optional<UserHandle>{UserHandle{"alice"}});
    ab_.canonicalizeMap["+491701234567"] = "+491701234567";
    Contact c;
    c.name = "Bob";
    c.projectIds = {ProjectId{"P1"}};
    ab_.nextLookup.push_back(std::optional<Contact>{c});

    ts_.nextFindOpenInProjectByCallerNumber.push_back(
        std::optional<Ticket>{}); // no open ticket for this caller
    ts_.nextCreate.push_back(TicketId{"T-new"});
    ts_.nextAddCallHandler.push_back(Result<void>{});

    auto uc = makeUseCase();
    auto r = sync(uc.run(ev()));

    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(ts_.created.size(), 1U);
    EXPECT_EQ(ts_.created[0].projectId, ProjectId{"P1"});
    EXPECT_EQ(ts_.created[0].subject, "Bob");
    EXPECT_EQ(ts_.created[0].callerNumber.v, "+491701234567");
    EXPECT_FALSE(ts_.created[0].calledNumber.has_value())
        << "outgoing tickets carry no calledNumber";
    ASSERT_TRUE(ts_.created[0].assignee.has_value());
    EXPECT_EQ(ts_.created[0].assignee->v, "alice");
    EXPECT_EQ(ts_.created[0].status, TicketStatus::InProgress)
        << "outgoing call is created actively-handled (InProgress), not New";
    ASSERT_EQ(ts_.addCallHandler_args.size(), 1U);
    EXPECT_EQ(ts_.addCallHandler_args[0].first, TicketId{"T-new"});
    EXPECT_EQ(ts_.addCallHandler_args[0].second.v, "alice");
}

TEST_F(HandleOutgoingCallTest, Incognito_AlwaysCreatesFreshWithIncognitoCaller) {
    ts_.nextResolveUser.push_back(std::optional<UserHandle>{UserHandle{"alice"}});
    ab_.defaultEmpty = true; // canonicalize -> empty
    ts_.nextCreate.push_back(TicketId{"T-new"});
    ts_.nextAddCallHandler.push_back(Result<void>{});

    auto uc = makeUseCase();
    auto r = sync(uc.run(ev()));

    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(ts_.findOpenInProjectBySubject_args.empty())
        << "incognito branch must not look up by subject (no dedup)";
    ASSERT_EQ(ts_.created.size(), 1U);
    EXPECT_EQ(ts_.created[0].projectId, ProjectId{"FB"});
    EXPECT_EQ(ts_.created[0].subject, "Incognito Caller");
    EXPECT_EQ(ts_.created[0].callerNumber.v, "Incognito")
        << "incognito outgoing must have callerNumber == \"Incognito\"";
    ASSERT_TRUE(ts_.created[0].assignee.has_value());
    EXPECT_EQ(ts_.created[0].assignee->v, "alice");
    EXPECT_EQ(ts_.created[0].status, TicketStatus::InProgress)
        << "incognito outgoing is also created InProgress (agent-placed call)";
    EXPECT_TRUE(ts_.saved.empty());
    EXPECT_TRUE(ab_.lookupCalls.empty()) << "incognito branch skips ab.lookup";
}

TEST_F(HandleOutgoingCallTest, Incognito_Replay_DoesNotDoubleCreateWhenTicketExists) {
    // Outgoing mirror: a replayed incognito outgoing event
    // must not double-create. On the replay path the use case dedups by exact
    // callid, re-records the operator (idempotent), and re-emits — no create.
    ts_.nextResolveUser.push_back(std::optional<UserHandle>{UserHandle{"alice"}});
    ab_.defaultEmpty = true; // canonicalize -> empty (incognito)
    auto existing = makeTicket(TicketId{"T-already"}, ProjectId{"FB"}, "Incognito Caller");
    existing.callIds = {CallId{"call-1"}};
    ts_.nextFindByExactCallid.push_back(std::optional<Ticket>{existing});
    ts_.nextAddCallHandler.push_back(Result<void>{});
    Ticket fresh;
    fresh.id = TicketId{"T-already"};
    fresh.status = TicketStatus::New;
    ts_.nextFetchById.push_back(Result<Ticket>{fresh});
    ts_.nextRecipientsFor.push_back(
        Result<std::vector<UserHandle>>{std::vector<UserHandle>{UserHandle{"alice"}}});

    auto uc = makeUseCase();
    auto r = sync(uc.run(ev(), /*replay=*/true));

    ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().message);
    ASSERT_EQ(ts_.findByExactCallid_args.size(), 1U);
    EXPECT_EQ(ts_.findByExactCallid_args[0], CallId{"call-1"});
    EXPECT_TRUE(ts_.created.empty()) << "replay must not double-create the incognito ticket";
    // The operator is re-recorded on the existing ticket (heals a crash between
    // create and addCallHandler).
    ASSERT_EQ(ts_.addCallHandler_args.size(), 1U);
    EXPECT_EQ(ts_.addCallHandler_args[0].first, TicketId{"T-already"});
    EXPECT_EQ(ts_.addCallHandler_args[0].second.v, "alice");
    ASSERT_EQ(ui_.ticketUpserts.size(), 1U);
    EXPECT_EQ(ui_.ticketUpserts[0].second.id, TicketId{"T-already"});
}

TEST_F(HandleOutgoingCallTest, Incognito_Replay_CreatesWhenNoPriorTicket) {
    // Replay but the prior run crashed before creating: lookup misses → create.
    ts_.nextResolveUser.push_back(std::optional<UserHandle>{UserHandle{"alice"}});
    ab_.defaultEmpty = true;
    ts_.nextFindByExactCallid.push_back(std::optional<Ticket>{}); // no prior ticket
    ts_.nextCreate.push_back(TicketId{"T-new"});
    ts_.nextAddCallHandler.push_back(Result<void>{});

    auto uc = makeUseCase();
    auto r = sync(uc.run(ev(), /*replay=*/true));

    ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().message);
    ASSERT_EQ(ts_.findByExactCallid_args.size(), 1U);
    ASSERT_EQ(ts_.created.size(), 1U);
    EXPECT_EQ(ts_.created[0].callerNumber.v, "Incognito");
}

TEST_F(HandleOutgoingCallTest, Incognito_LiveTwoCalls_StayUnDeduped_NoExactCallidLookup) {
    // Live path (replay == false): distinct incognito outgoing calls stay
    // distinct, and findByExactCallid is never consulted.
    ts_.nextResolveUser.push_back(std::optional<UserHandle>{UserHandle{"alice"}});
    ts_.nextResolveUser.push_back(std::optional<UserHandle>{UserHandle{"alice"}});
    ab_.defaultEmpty = true;
    ts_.nextCreate.push_back(TicketId{"T-a"});
    ts_.nextCreate.push_back(TicketId{"T-b"});
    ts_.nextAddCallHandler.push_back(Result<void>{});
    ts_.nextAddCallHandler.push_back(Result<void>{});

    auto uc = makeUseCase();
    ASSERT_TRUE(sync(uc.run(ev(), /*replay=*/false)).has_value());
    ASSERT_TRUE(sync(uc.run(ev(), /*replay=*/false)).has_value());

    EXPECT_EQ(ts_.created.size(), 2U) << "two live incognito calls => two tickets";
    EXPECT_TRUE(ts_.findByExactCallid_args.empty())
        << "live incognito path must not dedup by exact callid";
}

TEST_F(HandleOutgoingCallTest, ResolveUserNullopt_DropsEventSilently) {
    ts_.nextResolveUser.push_back(std::optional<UserHandle>{}); // user not found in OP

    auto uc = makeUseCase();
    auto r = sync(uc.run(ev()));

    ASSERT_TRUE(r.has_value()) << "must be non-fatal";
    EXPECT_TRUE(ts_.created.empty());
    EXPECT_TRUE(ts_.saved.empty());
    EXPECT_TRUE(ab_.canonicalizeCalls.empty()) << "must short-circuit before canonicalize";
    EXPECT_TRUE(ui_.invalidateScopes.empty());
}

TEST_F(HandleOutgoingCallTest, ResolveUserError_Propagates) {
    ts_.nextResolveUser.push_back(aid::plumbing::unexpected{
        Error{ErrorCode::UpstreamUnavailable, "openproject down", std::nullopt}});

    auto uc = makeUseCase();
    auto r = sync(uc.run(ev()));

    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ErrorCode::UpstreamUnavailable);
    EXPECT_TRUE(ui_.invalidateScopes.empty());
}

} // namespace
