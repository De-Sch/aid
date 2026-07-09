#include <gtest/gtest.h>

#include <chrono>
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
#include "aid/usecases/HandleIncomingCall.h"
#include "aid/value-types/CallEvent.h"
#include "aid/value-types/Contact.h"
#include "aid/value-types/Ids.h"
#include "aid/value-types/Ticket.h"

namespace {

using aid::CallId;
using aid::Contact;
using aid::IncomingCall;
using aid::PhoneNumber;
using aid::ProjectId;
using aid::Ticket;
using aid::TicketId;
using aid::TicketStatus;
using aid::crosscutting::Config;
using aid::fakes::FakeAddressBook;
using aid::fakes::FakeClock;
using aid::fakes::FakeTicketStore;
using aid::fakes::FakeUiNotifier;
using aid::plumbing::Error;
using aid::plumbing::ErrorCode;
using aid::plumbing::Result;
using aid::usecases::HandleIncomingCall;

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

class HandleIncomingCallTest : public ::testing::Test {
protected:
    FakeTicketStore ts_;
    FakeAddressBook ab_;
    FakeUiNotifier ui_;
    FakeClock clock_;
    Config::TicketRouting cfg_{ProjectId{"FB"}, "Incognito Caller"};

    HandleIncomingCall makeUseCase() { return HandleIncomingCall{ts_, ab_, ui_, clock_, cfg_}; }

    static IncomingCall ev() {
        return IncomingCall{CallId{"call-1"}, PhoneNumber{"+491701234567"}, PhoneNumber{"+4930"}};
    }

    // Awaits a Task<Result<void>> by spinning until done. Use cases never
    // suspend in tests (fakes are synchronous co_return), so this is bounded.
    static Result<void> sync(aid::plumbing::Task<Result<void>> task) {
        // Task is eager: it ran synchronously through the fake co_returns.
        // Result has already been written into the promise's optional.
        EXPECT_TRUE(task.done());
        // Re-enter await_resume to extract the value.
        struct Sentinel {
            bool await_ready() const noexcept { return true; }
            void await_suspend(std::coroutine_handle<>) const noexcept {}
            void await_resume() const noexcept {}
        };
        // Trampoline: a tiny coroutine to co_await the task and grab the
        // Result. Using a lambda + immediate invocation pattern.
        auto trampoline = [&]() -> aid::plumbing::Task<Result<void>> {
            co_return co_await std::move(task);
        };
        auto t = trampoline();
        EXPECT_TRUE(t.done());
        // Pop the value back out the same way the awaiter would.
        // (Task::await_resume is non-const; we need a fresh awaiter.)
        return [&]() -> Result<void> {
            struct Driver {
                aid::plumbing::Task<Result<void>>* tp;
                bool await_ready() const noexcept { return tp->done(); }
                void await_suspend(std::coroutine_handle<>) const noexcept {}
                Result<void> await_resume() { return tp->await_resume(); }
            };
            // We need to drive the awaiter externally; simpler: just use a
            // nested coroutine that ends by storing the result.
            std::optional<Result<void>> sink;
            auto pump = [&]() -> aid::plumbing::Task<Result<void>> {
                auto r = co_await std::move(t);
                sink = std::move(r);
                co_return Result<void>{};
            };
            auto p = pump();
            EXPECT_TRUE(p.done());
            return std::move(*sink);
        }();
    }
};

TEST_F(HandleIncomingCallTest, Incognito_AlwaysCreatesFreshTicketWithIncognitoCaller) {
    // Incognito calls are never deduped: each one opens a fresh ticket.
    // No by-subject lookup, no save, callerNumber == literal "Incognito".
    ab_.defaultEmpty = true; // canonicalize -> empty
    ts_.nextCreate.push_back(TicketId{"T-new"});
    // After create the use case re-fetches by id and emits a delta from the
    // authoritative (New) ticket.
    aid::Ticket created;
    created.id = TicketId{"T-new"};
    created.status = TicketStatus::New;
    ts_.nextFetchById.push_back(Result<aid::Ticket>{created});
    ts_.nextRecipientsFor.push_back(Result<std::vector<aid::UserHandle>>{
        std::vector<aid::UserHandle>{aid::UserHandle{"alice"}}});

    auto uc = makeUseCase();
    auto r = sync(uc.run(ev()));

    ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().message);
    EXPECT_TRUE(ts_.findOpenInProjectBySubject_args.empty())
        << "incognito branch must not look up by subject (no dedup)";
    ASSERT_EQ(ts_.created.size(), 1U);
    EXPECT_EQ(ts_.created[0].projectId, ProjectId{"FB"});
    EXPECT_EQ(ts_.created[0].subject, "Incognito Caller");
    EXPECT_EQ(ts_.created[0].callId, CallId{"call-1"});
    EXPECT_EQ(ts_.created[0].callerNumber.v, "Incognito")
        << "incognito caller number must be the literal \"Incognito\"";
    ASSERT_TRUE(ts_.created[0].calledNumber.has_value());
    EXPECT_EQ(ts_.created[0].calledNumber->v, "+4930");
    EXPECT_TRUE(ts_.saved.empty());
    EXPECT_TRUE(ab_.lookupCalls.empty()) << "incognito branch skips ab.lookup";
    // Live delta: the fresh New ticket upserts to each recipient.
    EXPECT_TRUE(ui_.ticketRemoves.empty());
    ASSERT_EQ(ui_.ticketUpserts.size(), 1U);
    EXPECT_EQ(ui_.ticketUpserts[0].first, aid::UserHandle{"alice"});
    EXPECT_EQ(ui_.ticketUpserts[0].second.id, TicketId{"T-new"});
}

TEST_F(HandleIncomingCallTest, Incognito_TwoCallsCreateTwoTickets) {
    // Two incognito calls => two creates, never merged into one.
    ab_.defaultEmpty = true;
    ts_.nextCreate.push_back(TicketId{"T-a"});
    ts_.nextCreate.push_back(TicketId{"T-b"});

    auto uc = makeUseCase();
    ASSERT_TRUE(sync(uc.run(ev())).has_value());
    ASSERT_TRUE(sync(uc.run(ev())).has_value());

    EXPECT_EQ(ts_.created.size(), 2U);
    EXPECT_TRUE(ts_.saved.empty());
}

TEST_F(HandleIncomingCallTest, Incognito_Replay_DoesNotDoubleCreateWhenTicketExists) {
    // Replaying an incognito event from the WAL after a
    // crash mid-truncate must NOT open a second ticket. On the replay path the
    // use case dedups by exact callid; the prior ticket is re-emitted instead.
    ab_.defaultEmpty = true; // canonicalize -> empty (incognito)
    aid::Ticket existing = makeTicket(TicketId{"T-already"}, ProjectId{"FB"}, "Incognito Caller");
    existing.callIds = {CallId{"call-1"}};
    ts_.nextFindByExactCallid.push_back(std::optional<aid::Ticket>{existing});
    ts_.nextRecipientsFor.push_back(Result<std::vector<aid::UserHandle>>{
        std::vector<aid::UserHandle>{aid::UserHandle{"alice"}}});

    auto uc = makeUseCase();
    auto r = sync(uc.run(ev(), /*replay=*/true));

    ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().message);
    ASSERT_EQ(ts_.findByExactCallid_args.size(), 1U);
    EXPECT_EQ(ts_.findByExactCallid_args[0], CallId{"call-1"});
    EXPECT_TRUE(ts_.created.empty()) << "replay must not double-create the incognito ticket";
    EXPECT_TRUE(ts_.saved.empty());
    // The existing ticket is re-pushed as a live upsert (idempotent).
    ASSERT_EQ(ui_.ticketUpserts.size(), 1U);
    EXPECT_EQ(ui_.ticketUpserts[0].second.id, TicketId{"T-already"});
}

TEST_F(HandleIncomingCallTest, Incognito_Replay_CreatesWhenNoPriorTicket) {
    // Replay but the prior run never got as far as creating the ticket: the
    // exact-callid lookup misses, so the incognito create proceeds as normal.
    ab_.defaultEmpty = true;
    ts_.nextFindByExactCallid.push_back(std::optional<aid::Ticket>{}); // no prior ticket
    ts_.nextCreate.push_back(TicketId{"T-new"});
    aid::Ticket created;
    created.id = TicketId{"T-new"};
    created.status = TicketStatus::New;
    ts_.nextFetchById.push_back(Result<aid::Ticket>{created});
    ts_.nextRecipientsFor.push_back(Result<std::vector<aid::UserHandle>>{
        std::vector<aid::UserHandle>{aid::UserHandle{"alice"}}});

    auto uc = makeUseCase();
    auto r = sync(uc.run(ev(), /*replay=*/true));

    ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().message);
    ASSERT_EQ(ts_.findByExactCallid_args.size(), 1U);
    ASSERT_EQ(ts_.created.size(), 1U);
    EXPECT_EQ(ts_.created[0].callerNumber.v, "Incognito");
}

TEST_F(HandleIncomingCallTest, Incognito_LiveTwoCalls_StayUnDeduped_NoExactCallidLookup) {
    // The live path (replay == false) must NOT consult findByExactCallid at all:
    // distinct live incognito calls remain distinct tickets.
    ab_.defaultEmpty = true;
    ts_.nextCreate.push_back(TicketId{"T-a"});
    ts_.nextCreate.push_back(TicketId{"T-b"});

    auto uc = makeUseCase();
    ASSERT_TRUE(sync(uc.run(ev(), /*replay=*/false)).has_value());
    ASSERT_TRUE(sync(uc.run(ev(), /*replay=*/false)).has_value());

    EXPECT_EQ(ts_.created.size(), 2U) << "two live incognito calls => two tickets";
    EXPECT_TRUE(ts_.findByExactCallid_args.empty())
        << "live incognito path must not dedup by exact callid";
}

TEST_F(HandleIncomingCallTest, Known_WithOpenTicket_Reuses) {
    ab_.canonicalizeMap["+491701234567"] = "+491701234567";
    Contact c;
    c.name = "Alice";
    c.projectIds = {ProjectId{"P1"}, ProjectId{"P2"}};
    ab_.nextLookup.push_back(std::optional<Contact>{c});

    // Known dedup is by caller number within each project: P1 has no open
    // ticket for this caller, P2 does → reuse P2's.
    ts_.nextFindOpenInProjectByCallerNumber.push_back(std::optional<Ticket>{});
    auto open = makeTicket(TicketId{"T-open"}, ProjectId{"P2"}, "Alice");
    ts_.nextFindOpenInProjectByCallerNumber.push_back(std::optional<Ticket>{open});
    ts_.nextSave.push_back(open);

    auto uc = makeUseCase();
    auto r = sync(uc.run(ev()));

    ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().message);
    ASSERT_EQ(ts_.findOpenInProjectByCallerNumber_args.size(), 2U);
    EXPECT_EQ(ts_.findOpenInProjectByCallerNumber_args[0].first, ProjectId{"P1"});
    EXPECT_EQ(ts_.findOpenInProjectByCallerNumber_args[0].second.v, "+491701234567");
    EXPECT_EQ(ts_.findOpenInProjectByCallerNumber_args[1].first, ProjectId{"P2"});
    EXPECT_TRUE(ts_.findLatestOpenCallInProject_args.empty())
        << "known branch dedups by caller number, not the project's rolling ticket";
    ASSERT_EQ(ts_.saved.size(), 1U);
    EXPECT_EQ(ts_.saved[0].id, TicketId{"T-open"});
    ASSERT_EQ(ts_.saved[0].callIds.size(), 1U);
    EXPECT_EQ(ts_.saved[0].callIds[0], CallId{"call-1"});
    EXPECT_TRUE(ts_.created.empty());
    ASSERT_EQ(ab_.lookupCalls.size(), 1U);
    EXPECT_EQ(ab_.lookupCalls[0].v, "+491701234567");
}

TEST_F(HandleIncomingCallTest, Known_NoOpenTicket_CreatesInFirstProject) {
    ab_.canonicalizeMap["+491701234567"] = "+491701234567";
    Contact c;
    c.name = "Alice";
    c.projectIds = {ProjectId{"P1"}};
    ab_.nextLookup.push_back(std::optional<Contact>{c});

    ts_.nextFindOpenInProjectByCallerNumber.push_back(
        std::optional<Ticket>{}); // no open ticket for this caller
    ts_.nextCreate.push_back(TicketId{"T-new"});

    auto uc = makeUseCase();
    auto r = sync(uc.run(ev()));

    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(ts_.created.size(), 1U);
    EXPECT_EQ(ts_.created[0].projectId, ProjectId{"P1"});
    EXPECT_EQ(ts_.created[0].subject, "Alice");
    EXPECT_EQ(ts_.created[0].callerNumber.v, "+491701234567")
        << "must be canonical number, not raw remote";
    EXPECT_TRUE(ts_.saved.empty());
}

TEST_F(HandleIncomingCallTest, Unknown_NoContact_CreatesInFallback) {
    ab_.canonicalizeMap["+491701234567"] = "+491701234567";
    ab_.nextLookup.push_back(std::optional<Contact>{});

    ts_.nextFindOpenInProjectByCallerNumber.push_back(std::optional<Ticket>{});
    ts_.nextCreate.push_back(TicketId{"T-new"});

    auto uc = makeUseCase();
    auto r = sync(uc.run(ev()));

    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(ts_.findOpenInProjectBySubject_args.empty())
        << "unknown branch dedups by number only — no by-subject lookup";
    ASSERT_EQ(ts_.created.size(), 1U);
    EXPECT_EQ(ts_.created[0].projectId, ProjectId{"FB"});
    EXPECT_EQ(ts_.created[0].callerNumber.v, "+491701234567");
    EXPECT_TRUE(ts_.saved.empty());
}

TEST_F(HandleIncomingCallTest, Unknown_FindsByNumber_Reuses) {
    ab_.canonicalizeMap["+491701234567"] = "+491701234567";
    ab_.nextLookup.push_back(std::optional<Contact>{});

    auto open = makeTicket(TicketId{"T-byNumber"}, ProjectId{"FB"});
    ts_.nextFindOpenInProjectByCallerNumber.push_back(std::optional<Ticket>{open});
    ts_.nextSave.push_back(open);

    auto uc = makeUseCase();
    auto r = sync(uc.run(ev()));

    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(ts_.findOpenInProjectBySubject_args.empty())
        << "unknown branch dedups by number only — no by-subject lookup";
    ASSERT_EQ(ts_.saved.size(), 1U);
    EXPECT_EQ(ts_.saved[0].id, TicketId{"T-byNumber"});
    EXPECT_TRUE(ts_.created.empty());
}

TEST_F(HandleIncomingCallTest, PortsError_TicketStoreCreateFailure_PropagatesNoUiNotify) {
    ab_.defaultEmpty = true;
    ts_.nextCreate.push_back(aid::plumbing::unexpected{
        Error{ErrorCode::UpstreamUnavailable, "openproject down", std::nullopt}});

    auto uc = makeUseCase();
    auto r = sync(uc.run(ev()));

    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ErrorCode::UpstreamUnavailable);
    EXPECT_TRUE(ui_.invalidateScopes.empty())
        << "must not notify the UI on a ports failure (use case stopped before step 5)";
}

TEST_F(HandleIncomingCallTest, AddressBookLookupFailure_Propagates) {
    ab_.canonicalizeMap["+491701234567"] = "+491701234567";
    ab_.nextLookup.push_back(aid::plumbing::unexpected{
        Error{ErrorCode::UpstreamTimeout, "carddav timeout", std::nullopt}});

    auto uc = makeUseCase();
    auto r = sync(uc.run(ev()));

    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ErrorCode::UpstreamTimeout);
    EXPECT_TRUE(ts_.created.empty());
    EXPECT_TRUE(ts_.saved.empty());
    EXPECT_TRUE(ui_.invalidateScopes.empty());
}

} // namespace
