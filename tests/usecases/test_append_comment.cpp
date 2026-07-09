#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "FakeTicketStore.h"
#include "FakeUiNotifier.h"
#include "aid/plumbing/ActionResult.h"
#include "aid/plumbing/Error.h"
#include "aid/plumbing/Result.h"
#include "aid/usecases/AppendComment.h"
#include "aid/value-types/Ids.h"
#include "aid/value-types/Ticket.h"

namespace {

using aid::ProjectId;
using aid::Ticket;
using aid::TicketId;
using aid::TicketStatus;
using aid::UserHandle;
using aid::fakes::FakeTicketStore;
using aid::fakes::FakeUiNotifier;
using aid::plumbing::ActionResult;
using aid::plumbing::Error;
using aid::plumbing::ErrorCode;
using aid::plumbing::Result;
using aid::usecases::AppendComment;

template <class T> Result<T> sync(aid::plumbing::Task<Result<T>> task) {
    std::optional<Result<T>> sink;
    auto pump = [&]() -> aid::plumbing::Task<Result<void>> {
        auto r = co_await std::move(task);
        sink.emplace(std::move(r));
        co_return Result<void>{};
    };
    auto p = pump();
    EXPECT_TRUE(p.done());
    return std::move(*sink);
}

Ticket makeTicket(TicketId id, std::string description) {
    Ticket t;
    t.id = std::move(id);
    t.projectId = ProjectId{"P1"};
    t.subject = "Alice";
    t.status = TicketStatus::InProgress;
    t.description = std::move(description);
    return t;
}

class AppendCommentTest : public ::testing::Test {
protected:
    FakeTicketStore ts_;
    FakeUiNotifier ui_;

    AppendComment makeUseCase() { return AppendComment{ts_, ui_}; }

    static TicketId id() { return TicketId{"T1"}; }
    static UserHandle viewer() { return UserHandle{"alice"}; }
};

TEST_F(AppendCommentTest, EmptyText_ReturnsActionResultOkFalse_NoStoreCalls) {
    auto uc = makeUseCase();
    auto r = sync(uc.run(id(), "   \t\n  ", viewer()));

    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(r->ok);
    EXPECT_EQ(r->op, "COMMENT_SAVE");
    EXPECT_EQ(r->ticketId, id());
    ASSERT_TRUE(r->message.has_value());
    EXPECT_EQ(*r->message, "empty comment");

    EXPECT_TRUE(ts_.fetchById_args.empty());
    EXPECT_TRUE(ts_.saved.empty());
    EXPECT_TRUE(ui_.actionResults.empty());
    EXPECT_TRUE(ui_.invalidateScopes.empty());
}

TEST_F(AppendCommentTest, TicketNotFound_ReturnsActionResultOkFalse_NoSave_NoNotify) {
    ts_.nextFetchById.push_back(
        aid::plumbing::unexpected{Error{ErrorCode::NotFound, "no such ticket", std::nullopt}});

    auto uc = makeUseCase();
    auto r = sync(uc.run(id(), "hello world", viewer()));

    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(r->ok);
    ASSERT_TRUE(r->message.has_value());
    EXPECT_EQ(*r->message, "ticket not found");

    EXPECT_TRUE(ts_.saved.empty());
    EXPECT_TRUE(ui_.actionResults.empty());
    EXPECT_TRUE(ui_.invalidateScopes.empty());
}

TEST_F(AppendCommentTest, HappyPath_AppendsTrimmedTextWithNewline_SavesAndNotifies) {
    auto t = makeTicket(id(), "first line");
    ts_.nextFetchById.push_back(t);
    // save()'s return value IS the emitter's delta source (post-save ticket).
    ts_.nextSave.push_back(t);
    ts_.nextRecipientsFor.push_back(
        Result<std::vector<UserHandle>>{std::vector<UserHandle>{viewer()}});

    auto uc = makeUseCase();
    auto r = sync(uc.run(id(), "  second line  ", viewer()));

    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->ok);
    EXPECT_EQ(r->op, "COMMENT_SAVE");
    EXPECT_FALSE(r->message.has_value());

    ASSERT_EQ(ts_.saved.size(), 1U);
    EXPECT_EQ(ts_.saved[0].description, "first line\nsecond line");

    ASSERT_EQ(ui_.actionResults.size(), 1U);
    EXPECT_EQ(ui_.actionResults[0].first, viewer());
    EXPECT_TRUE(ui_.actionResults[0].second.ok);
    // Live delta: open ticket upserts to each recipient (no remove).
    EXPECT_TRUE(ui_.ticketRemoves.empty());
    ASSERT_EQ(ui_.ticketUpserts.size(), 1U);
    EXPECT_EQ(ui_.ticketUpserts[0].first, viewer());
    EXPECT_EQ(ui_.ticketUpserts[0].second.id, id());
}

TEST_F(AppendCommentTest, SaveFails_PropagatesOuterError_NoNotify) {
    auto t = makeTicket(id(), "first line");
    ts_.nextFetchById.push_back(t);
    ts_.nextSave.push_back(
        aid::plumbing::unexpected{Error{ErrorCode::Conflict409, "lock version", std::nullopt}});

    auto uc = makeUseCase();
    auto r = sync(uc.run(id(), "hello", viewer()));

    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ErrorCode::Conflict409);
    EXPECT_TRUE(ui_.actionResults.empty());
    EXPECT_TRUE(ui_.invalidateScopes.empty());
}

TEST_F(AppendCommentTest, UnexpectedFetchError_PropagatesOuterError) {
    ts_.nextFetchById.push_back(aid::plumbing::unexpected{
        Error{ErrorCode::UpstreamTimeout, "openproject timeout", std::nullopt}});

    auto uc = makeUseCase();
    auto r = sync(uc.run(id(), "hello", viewer()));

    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ErrorCode::UpstreamTimeout);
    EXPECT_TRUE(ts_.saved.empty());
    EXPECT_TRUE(ui_.actionResults.empty());
}

} // namespace
