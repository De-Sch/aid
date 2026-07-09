#include <gtest/gtest.h>

#include <chrono>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "FakeClock.h"
#include "FakeTicketStore.h"
#include "FakeUiNotifier.h"
#include "aid/plumbing/Error.h"
#include "aid/plumbing/Result.h"
#include "aid/usecases/HandleHangup.h"
#include "aid/value-types/CallEvent.h"
#include "aid/value-types/Ids.h"
#include "aid/value-types/Ticket.h"

namespace {

using aid::CallId;
using aid::HangupCall;
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
using aid::usecases::HandleHangup;

Ticket makeTicket(TicketId id, std::vector<CallId> callIds = {CallId{"call-1"}},
                  std::string callLog = {}) {
    Ticket t;
    t.id = std::move(id);
    t.projectId = ProjectId{"P1"};
    t.subject = "Alice";
    t.status = TicketStatus::InProgress;
    t.assignee = UserHandle{"alice"};
    t.callIds = std::move(callIds);
    // Call-log lines live in callLength (the field hangup completes).
    t.callLength = std::move(callLog);
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

class HandleHangupTest : public ::testing::Test {
protected:
    FakeTicketStore ts_;
    FakeUiNotifier ui_;
    FakeClock clock_;

    HandleHangup makeUseCase() { return HandleHangup{ts_, ui_, clock_}; }

    static HangupCall ev() { return HangupCall{CallId{"call-1"}, PhoneNumber{"+491701234567"}}; }
};

TEST_F(HandleHangupTest, HappyPath_CompletesLine_RemovesCallid_StampsEnd) {
    clock_.now_ = aid::Timestamp{} + std::chrono::hours(24 * 365 * 56); // ~2026
    auto t = makeTicket(TicketId{"T1"}, {CallId{"call-1"}, CallId{"call-2"}},
                        "alice: Call start: 2026-05-20 10:00:00 (call-1)");
    ts_.nextFindByCallidContains.push_back(std::optional<Ticket>{t});
    ts_.nextSave.push_back(t);
    ts_.nextRecipientsFor.push_back(
        Result<std::vector<UserHandle>>{std::vector<UserHandle>{UserHandle{"alice"}}});

    auto uc = makeUseCase();
    auto r = sync(uc.run(ev()));

    ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().message);
    ASSERT_EQ(ts_.findByCallidContains_args.size(), 1U);
    EXPECT_EQ(ts_.findByCallidContains_args[0], CallId{"call-1"});
    ASSERT_EQ(ts_.saved.size(), 1U);
    ASSERT_TRUE(ts_.saved[0].callEnd.has_value());
    EXPECT_EQ(*ts_.saved[0].callEnd, clock_.now_);
    // call-1 must be removed; call-2 must remain.
    ASSERT_EQ(ts_.saved[0].callIds.size(), 1U);
    EXPECT_EQ(ts_.saved[0].callIds[0], CallId{"call-2"});
    // The callLength line should now carry the "Call End:" marker, with no
    // duration and with the now-useless "(call-1)" callid stripped on completion.
    EXPECT_NE(ts_.saved[0].callLength.find("Call End:"), std::string::npos);
    EXPECT_EQ(ts_.saved[0].callLength.find("Duration:"), std::string::npos);
    EXPECT_EQ(ts_.saved[0].callLength.find("(call-1)"), std::string::npos);
    EXPECT_EQ(ts_.saved[0].status, TicketStatus::InProgress)
        << "Hangup must NOT auto-close — agents close manually";
    // Live delta: an open (InProgress) ticket upserts to each recipient.
    EXPECT_TRUE(ui_.ticketRemoves.empty());
    ASSERT_EQ(ui_.ticketUpserts.size(), 1U);
    EXPECT_EQ(ui_.ticketUpserts[0].first, UserHandle{"alice"});
    EXPECT_EQ(ui_.ticketUpserts[0].second.id, TicketId{"T1"});
}

TEST_F(HandleHangupTest, NoTicket_IsCriticalError) {
    ts_.nextFindByCallidContains.push_back(std::optional<Ticket>{}); // nullopt

    auto uc = makeUseCase();
    auto r = sync(uc.run(ev()));

    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ErrorCode::InvariantViolation);
    EXPECT_NE(r.error().message.find("call-1"), std::string::npos)
        << "error message should name the offending callid";
    EXPECT_TRUE(ts_.saved.empty());
    EXPECT_TRUE(ui_.invalidateScopes.empty());
}

TEST_F(HandleHangupTest, LineMissing_StillSaves_StillStampsEnd_StillShrinksList) {
    clock_.now_ = aid::Timestamp{} + std::chrono::hours(24 * 365 * 56);
    auto t =
        makeTicket(TicketId{"T1"}, {CallId{"call-1"}}, "unrelated call-log with no matching line");
    const std::string before = t.callLength;
    ts_.nextFindByCallidContains.push_back(std::optional<Ticket>{t});
    ts_.nextSave.push_back(t);

    auto uc = makeUseCase();
    auto r = sync(uc.run(ev()));

    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(ts_.saved.size(), 1U);
    EXPECT_EQ(ts_.saved[0].callLength, before) << "no matching line means callLength is untouched";
    EXPECT_TRUE(ts_.saved[0].callIds.empty()) << "callid still removed from the list";
    EXPECT_TRUE(ts_.saved[0].callEnd.has_value());
}

TEST_F(HandleHangupTest, FindError_Propagates) {
    ts_.nextFindByCallidContains.push_back(aid::plumbing::unexpected{
        Error{ErrorCode::UpstreamTimeout, "openproject timeout", std::nullopt}});

    auto uc = makeUseCase();
    auto r = sync(uc.run(ev()));

    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ErrorCode::UpstreamTimeout);
    EXPECT_TRUE(ui_.invalidateScopes.empty());
}

TEST_F(HandleHangupTest, SaveError_Propagates_NoUiNotify) {
    clock_.now_ = aid::Timestamp{} + std::chrono::hours(24 * 365 * 56);
    auto t = makeTicket(TicketId{"T1"}, {CallId{"call-1"}},
                        "alice: Call start: 2026-05-20 10:00:00 (call-1)");
    ts_.nextFindByCallidContains.push_back(std::optional<Ticket>{t});
    ts_.nextSave.push_back(
        aid::plumbing::unexpected{Error{ErrorCode::Conflict409, "lock version", std::nullopt}});

    auto uc = makeUseCase();
    auto r = sync(uc.run(ev()));

    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ErrorCode::Conflict409);
    EXPECT_TRUE(ui_.invalidateScopes.empty());
}

} // namespace
