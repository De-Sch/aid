#include <gtest/gtest.h>

#include <chrono>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "FakeClock.h"
#include "FakeTicketStore.h"
#include "FakeUiNotifier.h"
#include "aid/domain/CallLineFormatter.h"
#include "aid/plumbing/Error.h"
#include "aid/plumbing/Result.h"
#include "aid/usecases/HandleAcceptedCall.h"
#include "aid/value-types/CallEvent.h"
#include "aid/value-types/Ids.h"
#include "aid/value-types/Ticket.h"

namespace {

using aid::AcceptedCall;
using aid::CallId;
using aid::PhoneNumber;
using aid::ProjectId;
using aid::Ticket;
using aid::TicketId;
using aid::TicketStatus;
using aid::UserHandle;
using aid::fakes::FakeClock;
using aid::fakes::FakeTicketStore;
using aid::fakes::FakeUiNotifier;
using aid::plumbing::Error;
using aid::plumbing::ErrorCode;
using aid::plumbing::Result;
using aid::usecases::HandleAcceptedCall;

Ticket makeTicket(TicketId id, TicketStatus status = TicketStatus::New,
                  std::optional<UserHandle> assignee = std::nullopt) {
    Ticket t;
    t.id = std::move(id);
    t.projectId = ProjectId{"P1"};
    t.subject = "Alice";
    t.status = status;
    t.assignee = std::move(assignee);
    return t;
}

// Pump a Task<Result<void>> to completion. Use cases never genuinely
// suspend in tests (fakes co_return synchronously).
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

class HandleAcceptedCallTest : public ::testing::Test {
protected:
    FakeTicketStore ts_;
    FakeUiNotifier ui_;
    FakeClock clock_;

    HandleAcceptedCall makeUseCase() { return HandleAcceptedCall{ts_, ui_, clock_}; }

    static AcceptedCall ev(std::optional<UserHandle> user = UserHandle{"alice"}) {
        return AcceptedCall{CallId{"call-1"}, PhoneNumber{"+491701234567"}, PhoneNumber{"+4930"},
                            std::move(user)};
    }
};

TEST_F(HandleAcceptedCallTest, HappyPath_SetsInProgress_CallStart_AppendsLine) {
    clock_.now_ = aid::Timestamp{} + std::chrono::hours(24 * 365 * 56); // ~2026
    auto t = makeTicket(TicketId{"T1"}, TicketStatus::New);
    t.description = "preexisting body";
    ts_.nextFindByCallidContains.push_back(std::optional<Ticket>{t});
    ts_.nextResolveUser.push_back(std::optional<UserHandle>{UserHandle{"alice"}});
    ts_.nextSave.push_back(t);
    ts_.nextAddCallHandler.push_back(Result<void>{});
    // Step 8 re-fetches by id and emits a delta from the authoritative
    // (now InProgress) ticket.
    ts_.nextFetchById.push_back(
        Result<Ticket>{makeTicket(TicketId{"T1"}, TicketStatus::InProgress)});
    ts_.nextRecipientsFor.push_back(
        Result<std::vector<UserHandle>>{std::vector<UserHandle>{UserHandle{"alice"}}});

    auto uc = makeUseCase();
    auto r = sync(uc.run(ev()));

    ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().message);
    ASSERT_EQ(ts_.findByCallidContains_args.size(), 1U);
    EXPECT_EQ(ts_.findByCallidContains_args[0], CallId{"call-1"});
    ASSERT_EQ(ts_.resolveUser_args.size(), 1U);
    EXPECT_EQ(ts_.resolveUser_args[0], "alice");
    ASSERT_EQ(ts_.saved.size(), 1U);
    EXPECT_EQ(ts_.saved[0].status, TicketStatus::InProgress);
    ASSERT_TRUE(ts_.saved[0].assignee.has_value());
    EXPECT_EQ(ts_.saved[0].assignee->v, "alice"); // ticket had no assignee → set
    ASSERT_EQ(ts_.addCallHandler_args.size(), 1U);
    EXPECT_EQ(ts_.addCallHandler_args[0].first, TicketId{"T1"});
    EXPECT_EQ(ts_.addCallHandler_args[0].second.v, "alice");
    ASSERT_TRUE(ts_.saved[0].callStart.has_value());
    EXPECT_EQ(*ts_.saved[0].callStart, clock_.now_);
    // The open line keyed by (callid) goes to callLength; the human-comment
    // description is left untouched.
    EXPECT_NE(ts_.saved[0].callLength.find("(call-1)"), std::string::npos);
    EXPECT_NE(ts_.saved[0].callLength.find("alice:"), std::string::npos);
    EXPECT_EQ(ts_.saved[0].callLength.find("preexisting body"), std::string::npos);
    EXPECT_EQ(ts_.saved[0].description, "preexisting body");
    // Live delta: re-fetched InProgress ticket upserts to each recipient.
    EXPECT_TRUE(ui_.ticketRemoves.empty());
    ASSERT_EQ(ui_.ticketUpserts.size(), 1U);
    EXPECT_EQ(ui_.ticketUpserts[0].first, UserHandle{"alice"});
    EXPECT_EQ(ui_.ticketUpserts[0].second.id, TicketId{"T1"});
}

// A different operator accepts a ticket that is already assigned. OpenProject
// allows a single assignee, so the existing assignee is preserved (no churn /
// 422) while the new operator is recorded in the callHandler CSV — the
// cross-project visibility mechanism. The call-log line must name the
// operator who actually answered THIS call (bob), not the ticket's sticky
// assignee (alice) — those diverge precisely in this scenario.
TEST_F(HandleAcceptedCallTest, AlreadyAssigned_KeepsAssignee_LineNamesAcceptingUser) {
    clock_.now_ = aid::Timestamp{} + std::chrono::hours(24 * 365 * 56);
    auto t = makeTicket(TicketId{"T1"}, TicketStatus::InProgress, UserHandle{"alice"});
    ts_.nextFindByCallidContains.push_back(std::optional<Ticket>{t});
    ts_.nextResolveUser.push_back(std::optional<UserHandle>{UserHandle{"bob"}});
    ts_.nextSave.push_back(t);
    ts_.nextAddCallHandler.push_back(Result<void>{});

    auto uc = makeUseCase();
    auto r = sync(uc.run(ev(UserHandle{"bob"})));

    ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().message);
    ASSERT_EQ(ts_.saved.size(), 1U);
    ASSERT_TRUE(ts_.saved[0].assignee.has_value());
    EXPECT_EQ(ts_.saved[0].assignee->v, "alice") << "existing assignee must not be overwritten";
    EXPECT_NE(ts_.saved[0].callLength.find("bob:"), std::string::npos)
        << "call line must name the accepting user, not the sticky assignee";
    EXPECT_EQ(ts_.saved[0].callLength.find("alice:"), std::string::npos)
        << "the old assignee must not appear in this call's line";
    ASSERT_EQ(ts_.addCallHandler_args.size(), 1U);
    EXPECT_EQ(ts_.addCallHandler_args[0].second.v, "bob") << "new operator recorded as handler";
}

// addCallHandler failure propagates (the CSV is the visibility mechanism; the
// WAL keeps the event for replay rather than the dashboard silently losing it).
TEST_F(HandleAcceptedCallTest, AddCallHandlerError_Propagates) {
    clock_.now_ = aid::Timestamp{} + std::chrono::hours(24 * 365 * 56);
    auto t = makeTicket(TicketId{"T1"}, TicketStatus::New);
    ts_.nextFindByCallidContains.push_back(std::optional<Ticket>{t});
    ts_.nextResolveUser.push_back(std::optional<UserHandle>{UserHandle{"alice"}});
    ts_.nextSave.push_back(t);
    ts_.nextAddCallHandler.push_back(aid::plumbing::unexpected{
        Error{ErrorCode::UpstreamUnavailable, "openproject down", std::nullopt}});

    auto uc = makeUseCase();
    auto r = sync(uc.run(ev()));

    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ErrorCode::UpstreamUnavailable);
    EXPECT_TRUE(ui_.invalidateScopes.empty()) << "no dashboard invalidate when recording failed";
}

TEST_F(HandleAcceptedCallTest, NoTicket_SkipsSaveAndNotify) {
    ts_.nextFindByCallidContains.push_back(std::optional<Ticket>{}); // nullopt
    auto uc = makeUseCase();
    auto r = sync(uc.run(ev()));

    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(ts_.saved.empty());
    EXPECT_TRUE(ts_.resolveUser_args.empty());
    EXPECT_TRUE(ui_.invalidateScopes.empty());
}

TEST_F(HandleAcceptedCallTest, ClosedTicket_StatusUnchanged) {
    clock_.now_ = aid::Timestamp{} + std::chrono::hours(24 * 365 * 56);
    auto t = makeTicket(TicketId{"T1"}, TicketStatus::Closed, UserHandle{"alice"});
    ts_.nextFindByCallidContains.push_back(std::optional<Ticket>{t});
    ts_.nextResolveUser.push_back(std::optional<UserHandle>{UserHandle{"alice"}});
    ts_.nextSave.push_back(t);
    ts_.nextAddCallHandler.push_back(Result<void>{});

    auto uc = makeUseCase();
    auto r = sync(uc.run(ev()));

    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(ts_.saved.size(), 1U);
    EXPECT_EQ(ts_.saved[0].status, TicketStatus::Closed) << "Closed tickets must not reopen";
}

TEST_F(HandleAcceptedCallTest, CallStart_OverwrittenOnEveryAccept) {
    const auto t0 = aid::Timestamp{} + std::chrono::hours(24 * 365 * 56);
    clock_.now_ = t0 + std::chrono::minutes(10);
    auto t = makeTicket(TicketId{"T1"}, TicketStatus::InProgress, UserHandle{"alice"});
    t.callStart = t0; // set by a prior Accept
    ts_.nextFindByCallidContains.push_back(std::optional<Ticket>{t});
    ts_.nextResolveUser.push_back(std::optional<UserHandle>{UserHandle{"alice"}});
    ts_.nextSave.push_back(t);
    ts_.nextAddCallHandler.push_back(Result<void>{});

    auto uc = makeUseCase();
    auto r = sync(uc.run(ev()));

    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(ts_.saved.size(), 1U);
    ASSERT_TRUE(ts_.saved[0].callStart.has_value());
    EXPECT_EQ(*ts_.saved[0].callStart, clock_.now_)
        << "callStart is updated on every accept; per-call history lives in callLength";
}

TEST_F(HandleAcceptedCallTest, SecondCallLineUsesItsOwnStartTime) {
    // A later call on a ticket that already handled an earlier call must stamp
    // its breadcrumb with ITS OWN accept time, not reuse the first call's start
    // (the multi-call start-time bug). The earlier line stays as history.
    const auto t0 = aid::Timestamp{} + std::chrono::hours(24 * 365 * 56);
    clock_.now_ = t0 + std::chrono::minutes(10);
    const std::string oldLine =
        "alice: Call start: 2020-01-01 00:00:00 Call End: 2020-01-01 00:05:00";
    auto t = makeTicket(TicketId{"T1"}, TicketStatus::InProgress, UserHandle{"alice"});
    t.callStart = t0;       // first call's start
    t.callLength = oldLine; // first call's completed breadcrumb
    ts_.nextFindByCallidContains.push_back(std::optional<Ticket>{t});
    ts_.nextResolveUser.push_back(std::optional<UserHandle>{UserHandle{"alice"}});
    ts_.nextSave.push_back(t);
    ts_.nextAddCallHandler.push_back(Result<void>{});

    auto uc = makeUseCase();
    auto r = sync(uc.run(ev())); // ev()'s callid is "call-1" (a new call)

    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(ts_.saved.size(), 1U);
    const auto& cl = ts_.saved[0].callLength;

    // The earlier call's line is preserved verbatim (history kept in callLength).
    EXPECT_NE(cl.find(oldLine), std::string::npos) << "first call's line preserved";

    // The new call-1 breadcrumb carries THIS accept's time (now), not t0.
    const auto sp = aid::domain::CallLineFormatter::findLineFor(cl, CallId{"call-1"});
    ASSERT_TRUE(sp.has_value());
    const auto line = cl.substr(sp->begin, sp->end - sp->begin);
    const auto parsed = aid::domain::CallLineFormatter::parseStart(line);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->start, clock_.now_)
        << "2nd call's breadcrumb uses its own start time, not the first call's";
    EXPECT_NE(*ts_.saved[0].callStart, t0) << "callStart advanced past the first call's time";
}

// The phone system omitted `user` on this Accepted-Call event. Never guess who's on the
// call from the ticket's (possibly stale) assignee — leave the line blank.
// The existing assignee itself is untouched (this event resolved no handler).
TEST_F(HandleAcceptedCallTest, NoUserField_NoLineAppended_AssigneeUntouched) {
    clock_.now_ = aid::Timestamp{} + std::chrono::hours(24 * 365 * 56);
    auto t = makeTicket(TicketId{"T1"}, TicketStatus::New, UserHandle{"bob"});
    ts_.nextFindByCallidContains.push_back(std::optional<Ticket>{t});
    ts_.nextSave.push_back(t);

    auto uc = makeUseCase();
    auto r = sync(uc.run(ev(std::nullopt))); // no user field

    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(ts_.resolveUser_args.empty()) << "no user → no resolveUser call";
    ASSERT_EQ(ts_.saved.size(), 1U);
    EXPECT_TRUE(ts_.saved[0].callLength.empty()) << "no resolved handler → no call-log line";
    ASSERT_TRUE(ts_.saved[0].assignee.has_value());
    EXPECT_EQ(ts_.saved[0].assignee->v, "bob") << "existing assignee left untouched";
}

// Ticket has no assignee yet and the phone system omitted `user`: no handler resolved,
// so neither the assignee nor a call-log line gets set — both stay empty.
TEST_F(HandleAcceptedCallTest, NoUserField_NoExistingAssignee_NothingSet) {
    clock_.now_ = aid::Timestamp{} + std::chrono::hours(24 * 365 * 56);
    auto t = makeTicket(TicketId{"T1"}, TicketStatus::New);
    ts_.nextFindByCallidContains.push_back(std::optional<Ticket>{t});
    ts_.nextSave.push_back(t);

    auto uc = makeUseCase();
    auto r = sync(uc.run(ev(std::nullopt))); // no user field

    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(ts_.resolveUser_args.empty()) << "no user → no resolveUser call";
    ASSERT_EQ(ts_.saved.size(), 1U);
    EXPECT_FALSE(ts_.saved[0].assignee.has_value()) << "no handler resolved → no assignee set";
    EXPECT_TRUE(ts_.saved[0].callLength.empty()) << "no handler resolved → no call-log line";
}

TEST_F(HandleAcceptedCallTest, DedupExistingLine_NoAppend) {
    clock_.now_ = aid::Timestamp{} + std::chrono::hours(24 * 365 * 56);
    auto t = makeTicket(TicketId{"T1"}, TicketStatus::InProgress, UserHandle{"alice"});
    t.callLength = "alice: Call start: 2026-05-20 10:00:00 (call-1)";
    const auto before = t.callLength;
    ts_.nextFindByCallidContains.push_back(std::optional<Ticket>{t});
    ts_.nextResolveUser.push_back(std::optional<UserHandle>{UserHandle{"alice"}});
    ts_.nextSave.push_back(t);
    ts_.nextAddCallHandler.push_back(Result<void>{});

    auto uc = makeUseCase();
    auto r = sync(uc.run(ev()));

    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(ts_.saved.size(), 1U);
    EXPECT_EQ(ts_.saved[0].callLength, before) << "existing line for (callid) must dedup";
}

TEST_F(HandleAcceptedCallTest, FindByCallidContainsError_PropagatesNoUiNotify) {
    ts_.nextFindByCallidContains.push_back(aid::plumbing::unexpected{
        Error{ErrorCode::UpstreamUnavailable, "openproject down", std::nullopt}});

    auto uc = makeUseCase();
    auto r = sync(uc.run(ev()));

    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ErrorCode::UpstreamUnavailable);
    EXPECT_TRUE(ui_.invalidateScopes.empty());
}

TEST_F(HandleAcceptedCallTest, ResolveUserNullopt_NoSave) {
    auto t = makeTicket(TicketId{"T1"});
    ts_.nextFindByCallidContains.push_back(std::optional<Ticket>{t});
    ts_.nextResolveUser.push_back(std::optional<UserHandle>{}); // user not found in OP

    auto uc = makeUseCase();
    auto r = sync(uc.run(ev()));

    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(ts_.saved.empty());
    EXPECT_TRUE(ui_.invalidateScopes.empty());
}

// A ticket whose callid field holds several comma-joined
// callids must still accept the second-and-later callids. The lookup is
// "contains", so the fake returns the multi-call ticket for call-1, and the
// accept must transition status + append the open call-start line.
TEST_F(HandleAcceptedCallTest, MultiCallidTicket_AcceptsSecondCallid_SetsInProgressAndLine) {
    clock_.now_ = aid::Timestamp{} + std::chrono::hours(24 * 365 * 56); // ~2026
    auto t = makeTicket(TicketId{"T1"}, TicketStatus::New);
    t.callIds = {CallId{"call-0"}, CallId{"call-1"}}; // two live calls on one ticket
    t.callLength = "alice: Call start: 2026-05-20 10:00:00 (call-0)";
    ts_.nextFindByCallidContains.push_back(std::optional<Ticket>{t});
    ts_.nextResolveUser.push_back(std::optional<UserHandle>{UserHandle{"alice"}});
    ts_.nextSave.push_back(t);
    ts_.nextAddCallHandler.push_back(Result<void>{});
    ts_.nextFetchById.push_back(
        Result<Ticket>{makeTicket(TicketId{"T1"}, TicketStatus::InProgress)});
    ts_.nextRecipientsFor.push_back(
        Result<std::vector<UserHandle>>{std::vector<UserHandle>{UserHandle{"alice"}}});

    auto uc = makeUseCase();
    auto r = sync(uc.run(ev())); // ev() defaults to callid call-1

    ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().message);
    ASSERT_EQ(ts_.findByCallidContains_args.size(), 1U);
    EXPECT_EQ(ts_.findByCallidContains_args[0], CallId{"call-1"});
    ASSERT_EQ(ts_.saved.size(), 1U);
    EXPECT_EQ(ts_.saved[0].status, TicketStatus::InProgress);
    // The open call-start line for the second callid is appended; the first
    // call's line is left intact. Both live in callLength.
    EXPECT_NE(ts_.saved[0].callLength.find("(call-1)"), std::string::npos);
    EXPECT_NE(ts_.saved[0].callLength.find("alice:"), std::string::npos);
    EXPECT_NE(ts_.saved[0].callLength.find("(call-0)"), std::string::npos);
    EXPECT_TRUE(ui_.ticketRemoves.empty());
    ASSERT_EQ(ui_.ticketUpserts.size(), 1U);
    EXPECT_EQ(ui_.ticketUpserts[0].second.id, TicketId{"T1"});
}

} // namespace
