#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "FakeAddressBook.h"
#include "FakeTicketStore.h"
#include "aid/plumbing/Error.h"
#include "aid/plumbing/Result.h"
#include "aid/usecases/GetDashboard.h"
#include "aid/value-types/Contact.h"
#include "aid/value-types/Dashboard.h"
#include "aid/value-types/Ids.h"

namespace {

using aid::AddressKind;
using aid::CallId;
using aid::Contact;
using aid::DashboardEntry;
using aid::DashboardView;
using aid::PhoneNumber;
using aid::TicketId;
using aid::TicketStatus;
using aid::UserHandle;
using aid::fakes::FakeAddressBook;
using aid::fakes::FakeTicketStore;
using aid::plumbing::Error;
using aid::plumbing::ErrorCode;
using aid::plumbing::Result;
using aid::usecases::GetDashboard;

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

DashboardEntry mkEntry(std::string id, std::string href, std::optional<CallId> active = {}) {
    DashboardEntry e;
    e.id = TicketId{std::move(id)};
    e.subject = "Alice";
    e.status = TicketStatus::InProgress;
    e.callerNumber = PhoneNumber{"+491701234567"};
    e.href = std::move(href);
    e.activeCallForViewer = std::move(active);
    return e;
}

Contact mkContact() {
    Contact c;
    c.name = "Bob";
    c.companyName = "ACME";
    c.kind = AddressKind::Person;
    c.phoneNumbers = {PhoneNumber{"+491701234567"}};
    c.projectIds = {aid::ProjectId{"support"}};
    return c;
}

class GetDashboardTest : public ::testing::Test {
protected:
    FakeTicketStore ts_;
    FakeAddressBook ab_;

    GetDashboard makeUseCase() { return GetDashboard{ts_, ab_}; }

    static UserHandle viewer() { return UserHandle{"alice"}; }
};

TEST_F(GetDashboardTest, NoActiveCall_SkipsAddressLookup_ReturnsTicketsOnly) {
    ts_.nextListDashboard.push_back(std::vector<DashboardEntry>{
        mkEntry("T1", "https://op.example/projects/support/work_packages/1")});

    auto uc = makeUseCase();
    auto r = sync(uc.run(viewer()));

    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(r->addressCallInformation.has_value());
    EXPECT_FALSE(r->active.has_value());
    ASSERT_EQ(r->tickets.size(), 1U);
    EXPECT_EQ(r->tickets[0].id, TicketId{"T1"});

    // No active call → no address-book lookup at all.
    EXPECT_TRUE(ab_.lookupCalls.empty());
    ASSERT_EQ(ts_.listDashboard_args.size(), 1U);
    EXPECT_EQ(ts_.listDashboard_args[0], viewer());
}

TEST_F(GetDashboardTest, ActiveCall_WithMatch_ReturnsContactHint) {
    ts_.nextListDashboard.push_back(std::vector<DashboardEntry>{
        mkEntry("T1", "https://op.example/projects/support/work_packages/1", CallId{"call-A"})});
    ab_.nextLookup.push_back(std::optional<Contact>{mkContact()});

    auto uc = makeUseCase();
    auto r = sync(uc.run(viewer()));

    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->active.has_value());
    ASSERT_TRUE(r->addressCallInformation.has_value());
    EXPECT_EQ(r->addressCallInformation->name, "Bob");
    EXPECT_EQ(r->addressCallInformation->companyName, "ACME");

    // The active call's caller number is canonicalized then looked up.
    ASSERT_EQ(ab_.lookupCalls.size(), 1U);
    EXPECT_EQ(ab_.lookupCalls[0], PhoneNumber{"+491701234567"});
}

TEST_F(GetDashboardTest, ActiveCall_NoMatch_LeavesHintNullopt) {
    ts_.nextListDashboard.push_back(std::vector<DashboardEntry>{
        mkEntry("T1", "https://op.example/projects/support/work_packages/1", CallId{"call-A"})});
    ab_.nextLookup.push_back(std::optional<Contact>{}); // clean "no match"

    auto uc = makeUseCase();
    auto r = sync(uc.run(viewer()));

    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->active.has_value());
    EXPECT_FALSE(r->addressCallInformation.has_value());
    EXPECT_EQ(ab_.lookupCalls.size(), 1U);
}

TEST_F(GetDashboardTest, ActiveCall_FirstHitWinsInOrder) {
    // Adapter ordering is New first, then InProgress, then updatedAt desc.
    // The use case must walk the list as the adapter gave it and pick the
    // FIRST entry with activeCallForViewer.
    ts_.nextListDashboard.push_back(std::vector<DashboardEntry>{
        mkEntry("T1", "https://op.example/projects/alpha/work_packages/1"),
        mkEntry("T2", "https://op.example/projects/beta/work_packages/2", CallId{"call-A"}),
        mkEntry("T3", "https://op.example/projects/gamma/work_packages/3", CallId{"call-B"})});
    ab_.nextLookup.push_back(std::optional<Contact>{});

    auto uc = makeUseCase();
    auto r = sync(uc.run(viewer()));

    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->active.has_value());
    EXPECT_EQ(r->active->ticketId, TicketId{"T2"}) << "first hit wins, not the last";
    EXPECT_EQ(r->active->callId, CallId{"call-A"});
    EXPECT_EQ(r->active->projectName, "beta");
    EXPECT_EQ(r->active->callerNumber, PhoneNumber{"+491701234567"});
}

TEST_F(GetDashboardTest, NoActiveCall_LeavesActiveNullopt) {
    ts_.nextListDashboard.push_back(std::vector<DashboardEntry>{
        mkEntry("T1", "https://op.example/projects/alpha/work_packages/1"),
        mkEntry("T2", "https://op.example/projects/beta/work_packages/2")});

    auto uc = makeUseCase();
    auto r = sync(uc.run(viewer()));

    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(r->active.has_value());
    EXPECT_EQ(r->tickets.size(), 2U);
}

TEST_F(GetDashboardTest, TicketStoreError_PropagatesAsOuterError) {
    ts_.nextListDashboard.push_back(aid::plumbing::unexpected{
        Error{ErrorCode::UpstreamUnavailable, "openproject 503", std::nullopt}});

    auto uc = makeUseCase();
    auto r = sync(uc.run(viewer()));

    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ErrorCode::UpstreamUnavailable);
}

TEST_F(GetDashboardTest, AddressBookError_PropagatesAsOuterError) {
    // Tickets list first (so the active call is known), then the address
    // lookup fails — the error must surface as the outer result.
    ts_.nextListDashboard.push_back(std::vector<DashboardEntry>{
        mkEntry("T1", "https://op.example/projects/support/work_packages/1", CallId{"call-A"})});
    ab_.nextLookup.push_back(aid::plumbing::unexpected{
        Error{ErrorCode::UpstreamTimeout, "davical timeout", std::nullopt}});

    auto uc = makeUseCase();
    auto r = sync(uc.run(viewer()));

    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ErrorCode::UpstreamTimeout);
}

TEST_F(GetDashboardTest, ProjectNameFromHref_ExtractsSegment) {
    ts_.nextListDashboard.push_back(std::vector<DashboardEntry>{
        mkEntry("T1", "https://op.example/projects/support/work_packages/42", CallId{"c"})});
    ab_.nextLookup.push_back(std::optional<Contact>{});

    auto uc = makeUseCase();
    auto r = sync(uc.run(viewer()));

    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->active.has_value());
    EXPECT_EQ(r->active->projectName, "support");
}

TEST_F(GetDashboardTest, ProjectNameFromHref_NoMarker_YieldsEmpty) {
    ts_.nextListDashboard.push_back(
        std::vector<DashboardEntry>{mkEntry("T1", "https://other/", CallId{"c"})});
    ab_.nextLookup.push_back(std::optional<Contact>{});

    auto uc = makeUseCase();
    auto r = sync(uc.run(viewer()));

    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->active.has_value());
    EXPECT_TRUE(r->active->projectName.empty());
}

} // namespace
