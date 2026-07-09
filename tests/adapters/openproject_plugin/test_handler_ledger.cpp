#include <gtest/gtest.h>

#include <vector>

#include "aid/adapters/openproject/internal/HandlerLedger.h"
#include "aid/value-types/Ids.h"

using aid::adapters::openproject::HandlerLedger;

namespace {

std::vector<aid::UserHandle> handlers(std::initializer_list<const char*> logins) {
    std::vector<aid::UserHandle> out;
    for (const char* l : logins)
        out.push_back(aid::UserHandle{l});
    return out;
}

std::vector<std::string> logins(const std::vector<aid::UserHandle>& hs) {
    std::vector<std::string> out;
    out.reserve(hs.size());
    for (const auto& h : hs)
        out.push_back(h.v);
    return out;
}

} // namespace

// exchange() on a ticket the ledger has never seen is the cold-start case: it
// returns nullopt (no prior set to diff against).
TEST(HandlerLedger, ExchangeUntrackedReturnsNullopt) {
    HandlerLedger ledger;
    auto prior = ledger.exchange(aid::TicketId{"1"}, handlers({"alice"}));
    EXPECT_FALSE(prior.has_value());
}

// record() then exchange() returns exactly the recorded set, and installs the
// new one (so the second exchange sees the first exchange's value).
TEST(HandlerLedger, ExchangeReturnsPriorRecordedSet) {
    HandlerLedger ledger;
    ledger.record(aid::TicketId{"1"}, handlers({"alice", "bob"}));

    auto prior = ledger.exchange(aid::TicketId{"1"}, handlers({"alice"}));
    ASSERT_TRUE(prior.has_value());
    EXPECT_EQ(logins(*prior), (std::vector<std::string>{"alice", "bob"}));

    // The exchange installed {alice}; a follow-up exchange must see it.
    auto prior2 = ledger.exchange(aid::TicketId{"1"}, handlers({}));
    ASSERT_TRUE(prior2.has_value());
    EXPECT_EQ(logins(*prior2), (std::vector<std::string>{"alice"}));
}

// record() overwrites (not merges) the known set.
TEST(HandlerLedger, RecordOverwritesPriorSet) {
    HandlerLedger ledger;
    ledger.record(aid::TicketId{"1"}, handlers({"alice", "bob"}));
    ledger.record(aid::TicketId{"1"}, handlers({"carol"}));

    auto prior = ledger.exchange(aid::TicketId{"1"}, handlers({"carol"}));
    ASSERT_TRUE(prior.has_value());
    EXPECT_EQ(logins(*prior), (std::vector<std::string>{"carol"}));
}

// Per-ticket isolation: one ticket's set never leaks into another's.
TEST(HandlerLedger, TicketsAreIsolated) {
    HandlerLedger ledger;
    ledger.record(aid::TicketId{"1"}, handlers({"alice"}));
    ledger.record(aid::TicketId{"2"}, handlers({"bob"}));

    auto p1 = ledger.exchange(aid::TicketId{"1"}, handlers({}));
    auto p2 = ledger.exchange(aid::TicketId{"2"}, handlers({}));
    ASSERT_TRUE(p1.has_value());
    ASSERT_TRUE(p2.has_value());
    EXPECT_EQ(logins(*p1), (std::vector<std::string>{"alice"}));
    EXPECT_EQ(logins(*p2), (std::vector<std::string>{"bob"}));
}

// An empty recorded set is a known baseline (e.g. a freshly created ticket),
// distinct from the cold-start nullopt.
TEST(HandlerLedger, EmptyRecordedSetIsKnownNotColdStart) {
    HandlerLedger ledger;
    ledger.record(aid::TicketId{"1"}, handlers({}));

    auto prior = ledger.exchange(aid::TicketId{"1"}, handlers({"alice"}));
    ASSERT_TRUE(prior.has_value()) << "empty baseline is still a known set";
    EXPECT_TRUE(prior->empty());
}

// recordIfAbsent() seeds a baseline on an untracked ticket — the find-arm
// cold-start warming path.
TEST(HandlerLedger, RecordIfAbsentSeedsWhenUntracked) {
    HandlerLedger ledger;
    ledger.recordIfAbsent(aid::TicketId{"1"}, handlers({"alice", "bob"}));

    auto prior = ledger.exchange(aid::TicketId{"1"}, handlers({"alice"}));
    ASSERT_TRUE(prior.has_value()) << "recordIfAbsent must seed a cold ticket";
    EXPECT_EQ(logins(*prior), (std::vector<std::string>{"alice", "bob"}));
}

// recordIfAbsent() must NOT clobber a live entry a write path already recorded —
// the guard that keeps find-arm warming free of the TOCTOU regression (a stale
// find-arm snapshot overwriting a fresher addCallHandler set).
TEST(HandlerLedger, RecordIfAbsentDefersToExistingLiveEntry) {
    HandlerLedger ledger;
    ledger.record(aid::TicketId{"1"}, handlers({"alice", "bob"}));  // fresh write-path set
    ledger.recordIfAbsent(aid::TicketId{"1"}, handlers({"alice"})); // staler find-arm snapshot

    auto prior = ledger.exchange(aid::TicketId{"1"}, handlers({}));
    ASSERT_TRUE(prior.has_value());
    EXPECT_EQ(logins(*prior), (std::vector<std::string>{"alice", "bob"}))
        << "the fresher recorded set must win; recordIfAbsent only fills gaps";
}
