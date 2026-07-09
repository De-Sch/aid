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
#include "aid/usecases/CloseTicket.h"
#include "aid/value-types/Ids.h"
#include "aid/value-types/Ticket.h"

namespace {

using aid::TicketId;
using aid::UserHandle;
using aid::fakes::FakeTicketStore;
using aid::fakes::FakeUiNotifier;
using aid::plumbing::ActionResult;
using aid::plumbing::Error;
using aid::plumbing::ErrorCode;
using aid::plumbing::Result;
using aid::usecases::CloseTicket;

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

class CloseTicketTest : public ::testing::Test {
protected:
    FakeTicketStore ts_;
    FakeUiNotifier ui_;

    CloseTicket makeUseCase() { return CloseTicket{ts_, ui_}; }

    static TicketId id() { return TicketId{"T1"}; }
    static UserHandle viewer() { return UserHandle{"alice"}; }
};

TEST_F(CloseTicketTest, HappyPath_NotifiesAndPushesRemove) {
    ts_.nextClose.push_back(Result<void>{});
    // After a successful close the use case re-fetches the now-Closed ticket and
    // the emitter turns it into a ticket_remove for each recipient.
    aid::Ticket closed;
    closed.id = id();
    closed.status = aid::TicketStatus::Closed;
    closed.lockVersion = 4;
    ts_.nextFetchById.push_back(Result<aid::Ticket>{closed});
    ts_.nextRecipientsFor.push_back(
        Result<std::vector<aid::UserHandle>>{std::vector<aid::UserHandle>{viewer()}});

    auto uc = makeUseCase();
    auto r = sync(uc.run(id(), viewer()));

    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->ok);
    EXPECT_EQ(r->op, "TICKET_CLOSE");
    EXPECT_EQ(r->ticketId, id());
    EXPECT_FALSE(r->message.has_value());

    ASSERT_EQ(ts_.closed.size(), 1U);
    EXPECT_EQ(ts_.closed[0], id());

    ASSERT_EQ(ui_.actionResults.size(), 1U);
    EXPECT_EQ(ui_.actionResults[0].first, viewer());
    EXPECT_TRUE(ui_.actionResults[0].second.ok);
    // A Closed ticket leaves the board: remove, never upsert.
    EXPECT_TRUE(ui_.ticketUpserts.empty());
    ASSERT_EQ(ui_.ticketRemoves.size(), 1U);
    EXPECT_EQ(std::get<0>(ui_.ticketRemoves[0]), viewer());
    EXPECT_EQ(std::get<1>(ui_.ticketRemoves[0]), id());
    EXPECT_EQ(std::get<2>(ui_.ticketRemoves[0]), 4);
}

TEST_F(CloseTicketTest, NotFound_ReturnsActionResultOkFalse_StillNotifies) {
    ts_.nextClose.push_back(
        aid::plumbing::unexpected{Error{ErrorCode::NotFound, "no such ticket", std::nullopt}});

    auto uc = makeUseCase();
    auto r = sync(uc.run(id(), viewer()));

    ASSERT_TRUE(r.has_value()); // expected failure → encoded in ActionResult, not outer Result
    EXPECT_FALSE(r->ok);
    EXPECT_EQ(r->op, "TICKET_CLOSE");
    ASSERT_TRUE(r->message.has_value());
    EXPECT_EQ(*r->message, "ticket not found");

    // UI is still notified on expected failures.
    ASSERT_EQ(ui_.actionResults.size(), 1U);
    EXPECT_FALSE(ui_.actionResults[0].second.ok);
    // The board didn't change, so no delta is pushed on a failed close.
    EXPECT_TRUE(ui_.ticketRemoves.empty());
    EXPECT_TRUE(ui_.ticketUpserts.empty());
}

TEST_F(CloseTicketTest, OtherError_ReturnsActionResultOkFalse_WithDetail) {
    ts_.nextClose.push_back(aid::plumbing::unexpected{
        Error{ErrorCode::UpstreamUnavailable, "openproject 503", std::nullopt}});

    auto uc = makeUseCase();
    auto r = sync(uc.run(id(), viewer()));

    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(r->ok);
    ASSERT_TRUE(r->message.has_value());
    EXPECT_EQ(r->message->substr(0, 14), "close failed: ");
    EXPECT_NE(r->message->find("openproject 503"), std::string::npos);

    ASSERT_EQ(ui_.actionResults.size(), 1U);
    EXPECT_TRUE(ui_.ticketRemoves.empty());
    EXPECT_TRUE(ui_.ticketUpserts.empty());
}

TEST_F(CloseTicketTest, IdempotentAlreadyClosed_AdapterReturnsOk) {
    // Adapter contract: StateTransitions::path(Closed, Closed) is empty →
    // close() returns success. Use case trusts that and reports ok.
    ts_.nextClose.push_back(Result<void>{});

    auto uc = makeUseCase();
    auto r = sync(uc.run(id(), viewer()));

    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->ok);
    EXPECT_FALSE(r->message.has_value());
}

} // namespace
