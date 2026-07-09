#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "FakeTicketStore.h"
#include "FakeUiNotifier.h"
#include "aid/plumbing/Error.h"
#include "aid/plumbing/Result.h"
#include "aid/plumbing/Task.h"
#include "aid/usecases/ReconcileMemberships.h"
#include "aid/value-types/Ids.h"
#include "aid/value-types/MembershipDelta.h"
#include "aid/value-types/Ticket.h"

namespace {

using aid::MembershipDelta;
using aid::ProjectId;
using aid::Ticket;
using aid::TicketId;
using aid::TicketStatus;
using aid::UserHandle;
using aid::fakes::FakeTicketStore;
using aid::fakes::FakeUiNotifier;
using aid::plumbing::Error;
using aid::plumbing::ErrorCode;
using aid::plumbing::Result;
using aid::usecases::ReconcileMemberships;

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

[[nodiscard]] Ticket makeTicket(int lockVersion, std::vector<UserHandle> handlers) {
    Ticket t;
    t.id = TicketId{"4242"};
    t.projectId = ProjectId{"7"};
    t.subject = "Acme GmbH";
    t.status = TicketStatus::InProgress;
    t.lockVersion = lockVersion;
    t.callHandlers = std::move(handlers);
    return t;
}

[[nodiscard]] MembershipDelta makeDelta(std::vector<UserHandle> added,
                                        std::vector<UserHandle> removed) {
    MembershipDelta d;
    d.project = ProjectId{"7"};
    d.added = std::move(added);
    d.removed = std::move(removed);
    return d;
}

class ReconcileMembershipsTest : public ::testing::Test {
protected:
    FakeTicketStore ts_;
    FakeUiNotifier ui_;

    ReconcileMemberships make() { return ReconcileMemberships{ts_, ui_}; }
};

TEST_F(ReconcileMembershipsTest, AddedMemberGetsUpsertPerOpenTicket) {
    ts_.nextOpenCallsInProject.push_back(
        Result<std::vector<Ticket>>{std::vector<Ticket>{makeTicket(3, {})}});

    auto rc = make();
    auto r = sync(rc.reconcile({makeDelta({UserHandle{"alice"}}, {})}));
    ASSERT_TRUE(r.has_value());

    // The heavy listing ran for exactly the changed project.
    ASSERT_EQ(ts_.openCallsInProject_args.size(), 1u);
    EXPECT_EQ(ts_.openCallsInProject_args.at(0), ProjectId{"7"});

    // One upsert for the added login, built per-viewer; no removes.
    ASSERT_EQ(ui_.ticketUpserts.size(), 1u);
    EXPECT_EQ(ui_.ticketUpserts.at(0).first, UserHandle{"alice"});
    EXPECT_EQ(ui_.ticketUpserts.at(0).second.id, TicketId{"4242"});
    EXPECT_TRUE(ui_.ticketRemoves.empty());
    ASSERT_EQ(ts_.buildEntry_args.size(), 1u);
    EXPECT_EQ(ts_.buildEntry_args.at(0).second, UserHandle{"alice"});
}

TEST_F(ReconcileMembershipsTest, RemovedNonHandlerGetsRemove) {
    ts_.nextOpenCallsInProject.push_back(
        Result<std::vector<Ticket>>{std::vector<Ticket>{makeTicket(9, {UserHandle{"carol"}})}});

    auto rc = make();
    auto r = sync(rc.reconcile({makeDelta({}, {UserHandle{"bob"}})}));
    ASSERT_TRUE(r.has_value());

    EXPECT_TRUE(ui_.ticketUpserts.empty());
    ASSERT_EQ(ui_.ticketRemoves.size(), 1u);
    EXPECT_EQ(std::get<0>(ui_.ticketRemoves.at(0)), UserHandle{"bob"});
    EXPECT_EQ(std::get<1>(ui_.ticketRemoves.at(0)), TicketId{"4242"});
    EXPECT_EQ(std::get<2>(ui_.ticketRemoves.at(0)), 9);
    // buildEntry is never touched on the pure-remove path.
    EXPECT_TRUE(ts_.buildEntry_args.empty());
}

TEST_F(ReconcileMembershipsTest, RemovedButStillHandlerEmitsNoRemove) {
    // bob was removed from the project but is still a recorded callHandler —
    // the cross-project handler arm keeps him visible, so NO remove.
    ts_.nextOpenCallsInProject.push_back(
        Result<std::vector<Ticket>>{std::vector<Ticket>{makeTicket(5, {UserHandle{"bob"}})}});

    auto rc = make();
    auto r = sync(rc.reconcile({makeDelta({}, {UserHandle{"bob"}})}));
    ASSERT_TRUE(r.has_value());

    EXPECT_TRUE(ui_.ticketUpserts.empty());
    EXPECT_TRUE(ui_.ticketRemoves.empty());
}

TEST_F(ReconcileMembershipsTest, EmptyOpenCallsPushesNoFrames) {
    ts_.nextOpenCallsInProject.push_back(Result<std::vector<Ticket>>{std::vector<Ticket>{}});

    auto rc = make();
    auto r = sync(rc.reconcile({makeDelta({UserHandle{"alice"}}, {UserHandle{"bob"}})}));
    ASSERT_TRUE(r.has_value());

    // Listing was consulted, but with no open tickets there is nothing to push.
    ASSERT_EQ(ts_.openCallsInProject_args.size(), 1u);
    EXPECT_TRUE(ui_.ticketUpserts.empty());
    EXPECT_TRUE(ui_.ticketRemoves.empty());
}

TEST_F(ReconcileMembershipsTest, ListingErrorIsBestEffortNotFatal) {
    // First project's listing fails; the second still reconciles.
    MembershipDelta bad = makeDelta({UserHandle{"alice"}}, {});
    bad.project = ProjectId{"7"};
    MembershipDelta good = makeDelta({UserHandle{"dora"}}, {});
    good.project = ProjectId{"8"};

    ts_.nextOpenCallsInProject.push_back(Result<std::vector<Ticket>>{aid::plumbing::unexpected{
        Error{ErrorCode::UpstreamUnavailable, "calls 503", std::nullopt}}});
    ts_.nextOpenCallsInProject.push_back(
        Result<std::vector<Ticket>>{std::vector<Ticket>{makeTicket(2, {})}});

    auto rc = make();
    auto r = sync(rc.reconcile({bad, good}));
    // Overall reconcile reports success despite the per-project failure.
    ASSERT_TRUE(r.has_value());

    // Both projects were attempted; only the good one produced a frame.
    ASSERT_EQ(ts_.openCallsInProject_args.size(), 2u);
    ASSERT_EQ(ui_.ticketUpserts.size(), 1u);
    EXPECT_EQ(ui_.ticketUpserts.at(0).first, UserHandle{"dora"});
}

} // namespace
