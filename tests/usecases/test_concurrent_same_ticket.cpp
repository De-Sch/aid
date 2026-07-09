#include <gtest/gtest.h>

#include <chrono>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "FakeClock.h"
#include "FakeTicketStore.h"
#include "FakeUiNotifier.h"
#include "aid/plumbing/Result.h"
#include "aid/usecases/AppendComment.h"
#include "aid/usecases/HandleAcceptedCall.h"
#include "aid/usecases/HandleHangup.h"
#include "aid/usecases/HandleTransferCall.h"
#include "aid/value-types/CallEvent.h"
#include "aid/value-types/Ids.h"
#include "aid/value-types/Ticket.h"

// Regression test at the USE-CASE layer.
//
// The per-callid mailbox serialises per callid, NOT per ticket, so two calls on
// one ticket produce events that run in parallel and both read-modify-write the
// same delta fields (callLength / callId / description). The fix makes every use
// case express its mutation as a PURE REDUCER applied to the ticket's CURRENT
// state inside save() (re-applied on the adapter's 409 refetch), rather than
// computing absolute field values from a snapshot and handing the whole ticket
// to save().
//
// These tests model the race deterministically: writer 1 runs to completion,
// then writer 2's fetch + save-seed are wired to writer 1's RESULT — exactly the
// state the adapter's 409 refetch would hand writer 2 after writer 1 landed. If
// a use case still computed its delta from its own pre-writer-1 snapshot (the
// old bug), writer 1's edit would be clobbered. With reducers, writer 2's delta
// lands on top of writer 1's edit and BOTH survive.

namespace {

using aid::AcceptedCall;
using aid::CallId;
using aid::HangupCall;
using aid::PhoneNumber;
using aid::ProjectId;
using aid::Ticket;
using aid::TicketId;
using aid::TicketStatus;
using aid::TransferCall;
using aid::UserHandle;
using aid::fakes::FakeClock;
using aid::fakes::FakeTicketStore;
using aid::fakes::FakeUiNotifier;
using aid::plumbing::Result;
using aid::usecases::AppendComment;
using aid::usecases::HandleAcceptedCall;
using aid::usecases::HandleHangup;
using aid::usecases::HandleTransferCall;

// Drive a Task<Result<T>> to completion synchronously (the fakes never truly
// suspend), returning the inner Result.
template <class R> R sync(aid::plumbing::Task<R> task) {
    std::optional<R> sink;
    auto pump = [&]() -> aid::plumbing::Task<Result<void>> {
        sink = co_await std::move(task);
        co_return Result<void>{};
    };
    auto p = pump();
    EXPECT_TRUE(p.done());
    return std::move(*sink);
}

Ticket baseTicket(std::vector<CallId> callIds, std::string callLength) {
    Ticket t;
    t.id = TicketId{"T1"};
    t.projectId = ProjectId{"P1"};
    t.subject = "Alice";
    t.status = TicketStatus::InProgress;
    t.assignee = UserHandle{"alice"};
    t.callIds = std::move(callIds);
    t.callLength = std::move(callLength);
    return t;
}

int countOccurrences(const std::string& haystack, const std::string& needle) {
    int n = 0;
    for (std::size_t pos = haystack.find(needle); pos != std::string::npos;
         pos = haystack.find(needle, pos + needle.size())) {
        ++n;
    }
    return n;
}

} // namespace

// Two concurrent Hangups (different callids) on one ticket: both lines must end
// with a "Call End:" marker and both callids must be removed.
TEST(ConcurrentSameTicket, HangupPlusHangup_BothLinesClosed_BothCallidsRemoved) {
    FakeTicketStore ts;
    FakeUiNotifier ui;
    FakeClock clock;
    clock.now_ = aid::Timestamp{} + std::chrono::hours(24 * 365 * 56); // ~2026

    Ticket s0 =
        baseTicket({CallId{"A"}, CallId{"B"}}, "alice: Call start: 2026-05-20 10:00:00 (A)\n"
                                               "bob: Call start: 2026-05-20 10:05:00 (B)");

    // Writer 1 — Hangup A.
    ts.nextFindByCallidContains.push_back(std::optional<Ticket>{s0});
    ts.nextSave.push_back(s0); // seed = fresh server state
    {
        HandleHangup uc{ts, ui, clock};
        auto r = sync(uc.run(HangupCall{CallId{"A"}, PhoneNumber{"+49170"}}));
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }
    ASSERT_EQ(ts.saved.size(), 1U);
    const Ticket s1 = ts.saved[0]; // A closed, callId now [B]

    // Writer 2 — Hangup B, racing: its fetch + save-seed see writer 1's result
    // (what the adapter's 409 refetch would return).
    ts.nextFindByCallidContains.push_back(std::optional<Ticket>{s1});
    ts.nextSave.push_back(s1);
    {
        HandleHangup uc{ts, ui, clock};
        auto r = sync(uc.run(HangupCall{CallId{"B"}, PhoneNumber{"+49170"}}));
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }
    ASSERT_EQ(ts.saved.size(), 2U);
    const Ticket& s2 = ts.saved[1];

    EXPECT_TRUE(s2.callIds.empty()) << "both callids removed";
    EXPECT_EQ(countOccurrences(s2.callLength, "Call End:"), 2)
        << "BOTH lines closed — writer 1's edit was not clobbered";
    EXPECT_EQ(s2.callLength.find("(A)"), std::string::npos) << "A's callid stripped on completion";
    EXPECT_EQ(s2.callLength.find("(B)"), std::string::npos) << "B's callid stripped on completion";
}

// Concurrent Hangup + Transfer on one ticket: the hangup's "Call End:" marker
// on its line must survive the transfer's rewrite of the OTHER line.
TEST(ConcurrentSameTicket, HangupPlusTransfer_HangupEditSurvivesTransfer) {
    FakeTicketStore ts;
    FakeUiNotifier ui;
    FakeClock clock;
    clock.now_ = aid::Timestamp{} + std::chrono::hours(24 * 365 * 56);

    Ticket s0 =
        baseTicket({CallId{"A"}, CallId{"B"}}, "alice: Call start: 2026-05-20 10:00:00 (A)\n"
                                               "bob: Call start: 2026-05-20 10:05:00 (B)");

    // Writer 1 — Hangup A.
    ts.nextFindByCallidContains.push_back(std::optional<Ticket>{s0});
    ts.nextSave.push_back(s0);
    {
        HandleHangup uc{ts, ui, clock};
        auto r = sync(uc.run(HangupCall{CallId{"A"}, PhoneNumber{"+49170"}}));
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }
    ASSERT_EQ(ts.saved.size(), 1U);
    const Ticket s1 = ts.saved[0];

    // Writer 2 — Transfer B to carol, racing against writer 1's result.
    ts.nextFindByCallidContains.push_back(std::optional<Ticket>{s1});
    ts.nextResolveUser.push_back(std::optional<UserHandle>{UserHandle{"carol"}});
    ts.nextSave.push_back(s1);
    ts.nextAddCallHandler.push_back(Result<void>{});
    {
        HandleTransferCall uc{ts, ui};
        auto r = sync(uc.run(TransferCall{CallId{"B"}, UserHandle{"carol"}}));
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }
    ASSERT_EQ(ts.saved.size(), 2U);
    const Ticket& s2 = ts.saved[1];

    EXPECT_NE(s2.callLength.find("Call End:"), std::string::npos)
        << "the hangup's Call End: marker survived the transfer's save";
    EXPECT_NE(s2.callLength.find("carol:"), std::string::npos) << "B's line rewritten to carol";
    EXPECT_EQ(s2.callLength.find("bob:"), std::string::npos) << "B's old operator rewritten away";
}

// Two concurrent Accepts (different callids) on one ticket: both start-lines
// must be present — neither append clobbers the other.
TEST(ConcurrentSameTicket, AcceptedPlusAccepted_BothStartLinesAppended) {
    FakeTicketStore ts;
    FakeUiNotifier ui;
    FakeClock clock;
    clock.now_ = aid::Timestamp{} + std::chrono::hours(24 * 365 * 56);

    Ticket s0 = baseTicket({CallId{"A"}, CallId{"B"}}, "");
    s0.assignee = std::nullopt; // first accept sets it

    // Writer 1 — Accept A as alice.
    ts.nextFindByCallidContains.push_back(std::optional<Ticket>{s0});
    ts.nextResolveUser.push_back(std::optional<UserHandle>{UserHandle{"alice"}});
    ts.nextSave.push_back(s0);
    ts.nextAddCallHandler.push_back(Result<void>{});
    {
        HandleAcceptedCall uc{ts, ui, clock};
        auto r = sync(uc.run(AcceptedCall{CallId{"A"}, PhoneNumber{"+49170"}, PhoneNumber{"+4930"},
                                          UserHandle{"alice"}}));
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }
    ASSERT_EQ(ts.saved.size(), 1U);
    const Ticket s1 = ts.saved[0]; // carries the (A) start-line

    // Writer 2 — Accept B as bob, racing against writer 1's result.
    ts.nextFindByCallidContains.push_back(std::optional<Ticket>{s1});
    ts.nextResolveUser.push_back(std::optional<UserHandle>{UserHandle{"bob"}});
    ts.nextSave.push_back(s1);
    ts.nextAddCallHandler.push_back(Result<void>{});
    {
        HandleAcceptedCall uc{ts, ui, clock};
        auto r = sync(uc.run(AcceptedCall{CallId{"B"}, PhoneNumber{"+49170"}, PhoneNumber{"+4930"},
                                          UserHandle{"bob"}}));
        ASSERT_TRUE(r.has_value()) << r.error().message;
    }
    ASSERT_EQ(ts.saved.size(), 2U);
    const Ticket& s2 = ts.saved[1];

    EXPECT_NE(s2.callLength.find("(A)"), std::string::npos)
        << "writer 1's start-line survived writer 2's append";
    EXPECT_NE(s2.callLength.find("(B)"), std::string::npos) << "writer 2's start-line appended";
    EXPECT_EQ(countOccurrences(s2.callLength, "Call start:"), 2) << "exactly two start-lines";
}

// Two concurrent AppendComments on one ticket: both comments must survive in
// `description` (no dropped comment).
TEST(ConcurrentSameTicket, AppendCommentPlusAppendComment_BothCommentsSurvive) {
    FakeTicketStore ts;
    FakeUiNotifier ui;

    Ticket s0 = baseTicket({CallId{"A"}}, "");
    s0.description = "base";

    // Writer 1 — comment "one".
    ts.nextFetchById.push_back(Result<Ticket>{s0});
    ts.nextSave.push_back(s0);
    {
        AppendComment uc{ts, ui};
        auto r = sync(uc.run(TicketId{"T1"}, "one", UserHandle{"alice"}));
        ASSERT_TRUE(r.has_value()) << r.error().message;
        EXPECT_TRUE(r->ok);
    }
    ASSERT_EQ(ts.saved.size(), 1U);
    const Ticket s1 = ts.saved[0]; // description "base\none"

    // Writer 2 — comment "two", racing against writer 1's result.
    ts.nextFetchById.push_back(Result<Ticket>{s1});
    ts.nextSave.push_back(s1);
    {
        AppendComment uc{ts, ui};
        auto r = sync(uc.run(TicketId{"T1"}, "two", UserHandle{"bob"}));
        ASSERT_TRUE(r.has_value()) << r.error().message;
        EXPECT_TRUE(r->ok);
    }
    ASSERT_EQ(ts.saved.size(), 2U);

    EXPECT_EQ(ts.saved[1].description, "base\none\ntwo")
        << "both comments survive — neither writer clobbered the other";
}
