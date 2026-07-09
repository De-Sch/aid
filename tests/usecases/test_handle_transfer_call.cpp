#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "FakeTicketStore.h"
#include "FakeUiNotifier.h"
#include "aid/plumbing/Error.h"
#include "aid/plumbing/Result.h"
#include "aid/usecases/HandleTransferCall.h"
#include "aid/value-types/CallEvent.h"
#include "aid/value-types/Ids.h"
#include "aid/value-types/Ticket.h"

namespace {

using aid::CallId;
using aid::ProjectId;
using aid::Ticket;
using aid::TicketId;
using aid::TicketStatus;
using aid::TransferCall;
using aid::UserHandle;
using aid::fakes::FakeTicketStore;
using aid::fakes::FakeUiNotifier;
using aid::plumbing::Error;
using aid::plumbing::ErrorCode;
using aid::plumbing::Result;
using aid::usecases::HandleTransferCall;

Ticket makeTicket(TicketId id, TicketStatus status = TicketStatus::InProgress,
                  UserHandle assignee = UserHandle{"alice"}, std::string callLog = {}) {
    Ticket t;
    t.id = std::move(id);
    t.projectId = ProjectId{"P1"};
    t.subject = "Alice";
    t.status = status;
    t.assignee = std::move(assignee);
    // Call-log lines live in callLength (the field the transfer rewrites).
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

class HandleTransferCallTest : public ::testing::Test {
protected:
    FakeTicketStore ts_;
    FakeUiNotifier ui_;

    HandleTransferCall makeUseCase() { return HandleTransferCall{ts_, ui_}; }

    static TransferCall ev() { return TransferCall{CallId{"call-1"}, UserHandle{"bob"}}; }
};

// A transfer rewrites the call-log line to the new operator and records them in
// the callHandler CSV, but does NOT churn an already-set assignee (OpenProject
// allows one assignee; the CSV is the cross-project visibility mechanism).
TEST_F(HandleTransferCallTest, HappyPath_RewritesUserPrefix_RecordsHandler_KeepsAssignee) {
    auto t = makeTicket(TicketId{"T1"}, TicketStatus::New, UserHandle{"alice"},
                        "alice: Call start: 2026-05-20 10:00:00 (call-1)");
    ts_.nextFindByCallidContains.push_back(std::optional<Ticket>{t});
    ts_.nextResolveUser.push_back(std::optional<UserHandle>{UserHandle{"bob"}});
    ts_.nextSave.push_back(t);
    ts_.nextAddCallHandler.push_back(Result<void>{});
    // After save + addCallHandler the use case re-fetches by id and emits.
    ts_.nextFetchById.push_back(Result<Ticket>{
        makeTicket(TicketId{"T1"}, TicketStatus::InProgress, UserHandle{"alice"}, "")});
    ts_.nextRecipientsFor.push_back(
        Result<std::vector<UserHandle>>{std::vector<UserHandle>{UserHandle{"bob"}}});

    auto uc = makeUseCase();
    auto r = sync(uc.run(ev()));

    ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().message);
    ASSERT_EQ(ts_.findByCallidContains_args.size(), 1U);
    EXPECT_EQ(ts_.findByCallidContains_args[0], CallId{"call-1"});
    ASSERT_EQ(ts_.saved.size(), 1U);
    EXPECT_EQ(ts_.saved[0].status, TicketStatus::InProgress);
    ASSERT_TRUE(ts_.saved[0].assignee.has_value());
    EXPECT_EQ(ts_.saved[0].assignee->v, "alice") << "existing assignee preserved (no churn/422)";
    ASSERT_EQ(ts_.addCallHandler_args.size(), 1U);
    EXPECT_EQ(ts_.addCallHandler_args[0].first, TicketId{"T1"});
    EXPECT_EQ(ts_.addCallHandler_args[0].second.v, "bob") << "new operator recorded as handler";
    EXPECT_NE(ts_.saved[0].callLength.find("bob:"), std::string::npos);
    EXPECT_EQ(ts_.saved[0].callLength.find("alice:"), std::string::npos)
        << "alice's prefix must be replaced";
    EXPECT_NE(ts_.saved[0].callLength.find("(call-1)"), std::string::npos)
        << "callid breadcrumb preserved";
    // Live delta: re-fetched InProgress ticket upserts to each recipient.
    EXPECT_TRUE(ui_.ticketRemoves.empty());
    ASSERT_EQ(ui_.ticketUpserts.size(), 1U);
    EXPECT_EQ(ui_.ticketUpserts[0].first, UserHandle{"bob"});
    EXPECT_EQ(ui_.ticketUpserts[0].second.id, TicketId{"T1"});
}

// When the ticket has NO assignee, the transferred-to operator becomes it.
TEST_F(HandleTransferCallTest, UnassignedTicket_TransferSetsAssignee) {
    Ticket t;
    t.id = TicketId{"T1"};
    t.projectId = ProjectId{"P1"};
    t.subject = "Alice";
    t.status = TicketStatus::New; // no assignee
    ts_.nextFindByCallidContains.push_back(std::optional<Ticket>{t});
    ts_.nextResolveUser.push_back(std::optional<UserHandle>{UserHandle{"bob"}});
    ts_.nextSave.push_back(t);
    ts_.nextAddCallHandler.push_back(Result<void>{});

    auto uc = makeUseCase();
    auto r = sync(uc.run(ev()));

    ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().message);
    ASSERT_EQ(ts_.saved.size(), 1U);
    ASSERT_TRUE(ts_.saved[0].assignee.has_value());
    EXPECT_EQ(ts_.saved[0].assignee->v, "bob") << "empty assignee → set to the transferred-to user";
}

TEST_F(HandleTransferCallTest, NoTicket_SkipsSaveAndNotify) {
    ts_.nextFindByCallidContains.push_back(std::optional<Ticket>{});

    auto uc = makeUseCase();
    auto r = sync(uc.run(ev()));

    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(ts_.saved.empty());
    EXPECT_TRUE(ts_.resolveUser_args.empty());
    EXPECT_TRUE(ui_.invalidateScopes.empty());
}

TEST_F(HandleTransferCallTest, ClosedTicket_StatusUnchanged_StillRewritesLine) {
    auto t = makeTicket(TicketId{"T1"}, TicketStatus::Closed, UserHandle{"alice"},
                        "alice: Call start: 2026-05-20 10:00:00 (call-1)");
    ts_.nextFindByCallidContains.push_back(std::optional<Ticket>{t});
    ts_.nextResolveUser.push_back(std::optional<UserHandle>{UserHandle{"bob"}});
    ts_.nextSave.push_back(t);
    ts_.nextAddCallHandler.push_back(Result<void>{});

    auto uc = makeUseCase();
    auto r = sync(uc.run(ev()));

    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(ts_.saved.size(), 1U);
    EXPECT_EQ(ts_.saved[0].status, TicketStatus::Closed);
    EXPECT_NE(ts_.saved[0].callLength.find("bob:"), std::string::npos);
}

TEST_F(HandleTransferCallTest, ResolveUserNullopt_NoSave) {
    auto t = makeTicket(TicketId{"T1"});
    ts_.nextFindByCallidContains.push_back(std::optional<Ticket>{t});
    ts_.nextResolveUser.push_back(std::optional<UserHandle>{}); // user not in OP

    auto uc = makeUseCase();
    auto r = sync(uc.run(ev()));

    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(ts_.saved.empty());
    EXPECT_TRUE(ui_.invalidateScopes.empty());
}

TEST_F(HandleTransferCallTest, LineNotFound_AssigneeUpdated_NoRewrite) {
    auto t = makeTicket(TicketId{"T1"}, TicketStatus::InProgress, UserHandle{"alice"},
                        "unrelated call-log with no matching line");
    const std::string before = t.callLength;
    ts_.nextFindByCallidContains.push_back(std::optional<Ticket>{t});
    ts_.nextResolveUser.push_back(std::optional<UserHandle>{UserHandle{"bob"}});
    ts_.nextSave.push_back(t);
    ts_.nextAddCallHandler.push_back(Result<void>{});

    auto uc = makeUseCase();
    auto r = sync(uc.run(ev()));

    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(ts_.saved.size(), 1U);
    EXPECT_EQ(ts_.saved[0].callLength, before) << "no matching line means callLength is untouched";
    ASSERT_TRUE(ts_.saved[0].assignee.has_value());
    EXPECT_EQ(ts_.saved[0].assignee->v, "alice") << "existing assignee preserved";
    ASSERT_EQ(ts_.addCallHandler_args.size(), 1U);
    EXPECT_EQ(ts_.addCallHandler_args[0].second.v, "bob") << "new operator still recorded";
}

TEST_F(HandleTransferCallTest, FindError_Propagates) {
    ts_.nextFindByCallidContains.push_back(aid::plumbing::unexpected{
        Error{ErrorCode::UpstreamTimeout, "openproject timeout", std::nullopt}});

    auto uc = makeUseCase();
    auto r = sync(uc.run(ev()));

    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ErrorCode::UpstreamTimeout);
    EXPECT_TRUE(ui_.invalidateScopes.empty());
}

} // namespace
