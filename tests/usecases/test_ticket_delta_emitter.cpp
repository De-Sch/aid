#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "FakeTicketStore.h"
#include "FakeUiNotifier.h"
#include "aid/plumbing/Error.h"
#include "aid/plumbing/Result.h"
#include "aid/plumbing/Task.h"
#include "aid/usecases/TicketDeltaEmitter.h"
#include "aid/value-types/Ids.h"
#include "aid/value-types/Ticket.h"
#include "aid/value-types/WebhookDecode.h"

namespace {

using aid::Ticket;
using aid::TicketId;
using aid::TicketStatus;
using aid::UserHandle;
using aid::WebhookDecode;
using aid::fakes::FakeTicketStore;
using aid::fakes::FakeUiNotifier;
using aid::plumbing::Error;
using aid::plumbing::ErrorCode;
using aid::plumbing::Result;
using aid::usecases::TicketDeltaEmitter;

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

[[nodiscard]] Ticket makeTicket(TicketStatus status, int lockVersion) {
    Ticket t;
    t.id = TicketId{"4242"};
    t.subject = "Acme GmbH";
    t.status = status;
    t.lockVersion = lockVersion;
    return t;
}

class TicketDeltaEmitterTest : public ::testing::Test {
protected:
    FakeTicketStore ts_;
    FakeUiNotifier ui_;

    TicketDeltaEmitter makeEmitter() { return TicketDeltaEmitter{ts_, ui_}; }
};

TEST_F(TicketDeltaEmitterTest, FansUpsertOutToEveryRecipientOnly) {
    ts_.nextRecipientsFor.push_back(Result<std::vector<UserHandle>>{
        std::vector<UserHandle>{UserHandle{"alice"}, UserHandle{"bob"}}});

    auto em = makeEmitter();
    auto r = sync(em.emitTicketDelta(makeTicket(TicketStatus::InProgress, 3)));
    ASSERT_TRUE(r.has_value());

    // recipientsFor was consulted with the emitted ticket.
    ASSERT_EQ(ts_.recipientsFor_args.size(), 1u);
    EXPECT_EQ(ts_.recipientsFor_args.at(0).id, TicketId{"4242"});

    // One upsert per recipient, and ONLY to recipients — no removes.
    ASSERT_EQ(ui_.ticketUpserts.size(), 2u);
    EXPECT_TRUE(ui_.ticketRemoves.empty());
    EXPECT_EQ(ui_.ticketUpserts.at(0).first, UserHandle{"alice"});
    EXPECT_EQ(ui_.ticketUpserts.at(1).first, UserHandle{"bob"});
    // buildEntry was invoked per recipient (per-viewer projection).
    ASSERT_EQ(ts_.buildEntry_args.size(), 2u);
    EXPECT_EQ(ts_.buildEntry_args.at(0).second, UserHandle{"alice"});
    EXPECT_EQ(ts_.buildEntry_args.at(1).second, UserHandle{"bob"});
}

TEST_F(TicketDeltaEmitterTest, UpsertEntryCarriesTicketLockVersion) {
    ts_.nextRecipientsFor.push_back(
        Result<std::vector<UserHandle>>{std::vector<UserHandle>{UserHandle{"alice"}}});

    auto em = makeEmitter();
    auto r = sync(em.emitTicketDelta(makeTicket(TicketStatus::New, 7)));
    ASSERT_TRUE(r.has_value());

    ASSERT_EQ(ui_.ticketUpserts.size(), 1u);
    // The version field must be present and reflect the POST-SAVE lockVersion.
    EXPECT_EQ(ui_.ticketUpserts.at(0).second.lockVersion, 7);
    EXPECT_EQ(ui_.ticketUpserts.at(0).second.id, TicketId{"4242"});
}

TEST_F(TicketDeltaEmitterTest, ClosedTicketEmitsRemoveNotUpsert) {
    ts_.nextRecipientsFor.push_back(
        Result<std::vector<UserHandle>>{std::vector<UserHandle>{UserHandle{"alice"}}});

    auto em = makeEmitter();
    auto r = sync(em.emitTicketDelta(makeTicket(TicketStatus::Closed, 9)));
    ASSERT_TRUE(r.has_value());

    EXPECT_TRUE(ui_.ticketUpserts.empty());
    ASSERT_EQ(ui_.ticketRemoves.size(), 1u);
    EXPECT_EQ(std::get<0>(ui_.ticketRemoves.at(0)), UserHandle{"alice"});
    EXPECT_EQ(std::get<1>(ui_.ticketRemoves.at(0)), TicketId{"4242"});
    EXPECT_EQ(std::get<2>(ui_.ticketRemoves.at(0)), 9);
    // buildEntry is never called on the remove path.
    EXPECT_TRUE(ts_.buildEntry_args.empty());
}

TEST_F(TicketDeltaEmitterTest, NoRecipientsEmitsNothing) {
    ts_.nextRecipientsFor.push_back(Result<std::vector<UserHandle>>{std::vector<UserHandle>{}});

    auto em = makeEmitter();
    auto r = sync(em.emitTicketDelta(makeTicket(TicketStatus::InProgress, 2)));
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(ui_.ticketUpserts.empty());
    EXPECT_TRUE(ui_.ticketRemoves.empty());
}

TEST_F(TicketDeltaEmitterTest, RecipientsForErrorPropagatesAndPushesNothing) {
    ts_.nextRecipientsFor.push_back(Result<std::vector<UserHandle>>{aid::plumbing::unexpected{
        Error{ErrorCode::UpstreamUnavailable, "members 503", std::nullopt}}});

    auto em = makeEmitter();
    auto r = sync(em.emitTicketDelta(makeTicket(TicketStatus::InProgress, 2)));
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ErrorCode::UpstreamUnavailable);
    EXPECT_TRUE(ui_.ticketUpserts.empty());
    EXPECT_TRUE(ui_.ticketRemoves.empty());
}

// ---- emitWebhookDelta (Phase 6 / S8): upsert fan-out + handler-drop removes ----

TEST_F(TicketDeltaEmitterTest, WebhookDeltaDroppedRecipientGetsRemoveFrame) {
    // Current recipients still get the usual upsert; the admin-dropped login —
    // surfaced by decodeWebhook, disjoint from the current set — gets a remove.
    ts_.nextRecipientsFor.push_back(
        Result<std::vector<UserHandle>>{std::vector<UserHandle>{UserHandle{"alice"}}});

    WebhookDecode decode;
    decode.ticket = makeTicket(TicketStatus::InProgress, 5);
    decode.droppedRecipients = {UserHandle{"carol"}};

    auto em = makeEmitter();
    auto r = sync(em.emitWebhookDelta(std::move(decode)));
    ASSERT_TRUE(r.has_value());

    // The current recipient was upserted (the plain emit path is unchanged).
    ASSERT_EQ(ui_.ticketUpserts.size(), 1u);
    EXPECT_EQ(ui_.ticketUpserts.at(0).first, UserHandle{"alice"});

    // The dropped recipient got exactly one remove carrying the ticket's id +
    // post-save lockVersion (the viewer's stale-frame discriminator).
    ASSERT_EQ(ui_.ticketRemoves.size(), 1u);
    EXPECT_EQ(std::get<0>(ui_.ticketRemoves.at(0)), UserHandle{"carol"});
    EXPECT_EQ(std::get<1>(ui_.ticketRemoves.at(0)), TicketId{"4242"});
    EXPECT_EQ(std::get<2>(ui_.ticketRemoves.at(0)), 5);
}

TEST_F(TicketDeltaEmitterTest, WebhookDeltaNoDroppedIsPlainEmit) {
    // No handler dropped: behaviour is identical to emitTicketDelta — upserts to
    // every current recipient and NOT a single extra remove.
    ts_.nextRecipientsFor.push_back(Result<std::vector<UserHandle>>{
        std::vector<UserHandle>{UserHandle{"alice"}, UserHandle{"bob"}}});

    WebhookDecode decode;
    decode.ticket = makeTicket(TicketStatus::InProgress, 3);
    // decode.droppedRecipients deliberately left empty.

    auto em = makeEmitter();
    auto r = sync(em.emitWebhookDelta(std::move(decode)));
    ASSERT_TRUE(r.has_value());

    ASSERT_EQ(ui_.ticketUpserts.size(), 2u);
    EXPECT_EQ(ui_.ticketUpserts.at(0).first, UserHandle{"alice"});
    EXPECT_EQ(ui_.ticketUpserts.at(1).first, UserHandle{"bob"});
    EXPECT_TRUE(ui_.ticketRemoves.empty());
}

TEST_F(TicketDeltaEmitterTest, WebhookDeltaRemovesFireEvenWhenUpsertFanoutFails) {
    // The dropped removes come from decodeWebhook, not recipientsFor, so they must
    // still fire when the upsert fan-out errors (recipientsFor down). The error
    // still propagates so the caller can log it.
    ts_.nextRecipientsFor.push_back(Result<std::vector<UserHandle>>{aid::plumbing::unexpected{
        Error{ErrorCode::UpstreamUnavailable, "members 503", std::nullopt}}});

    WebhookDecode decode;
    decode.ticket = makeTicket(TicketStatus::InProgress, 8);
    decode.droppedRecipients = {UserHandle{"carol"}};

    auto em = makeEmitter();
    auto r = sync(em.emitWebhookDelta(std::move(decode)));
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ErrorCode::UpstreamUnavailable);

    EXPECT_TRUE(ui_.ticketUpserts.empty());
    ASSERT_EQ(ui_.ticketRemoves.size(), 1u);
    EXPECT_EQ(std::get<0>(ui_.ticketRemoves.at(0)), UserHandle{"carol"});
}

} // namespace
