#include <gtest/gtest.h>

#include "aid/adapters/openproject/internal/ProducedLedger.h"
#include "aid/value-types/Ids.h"

namespace {

using aid::TicketId;
using aid::adapters::openproject::ProducedLedger;

// The core Phase-6 echo-suppression predicate: a produced (id, version) pair is
// recognised, and ONLY that exact pair — never a neighbouring version, which is
// what distinguishes a self-echo from a later human edit at V+1/V+2.

TEST(ProducedLedger, RecordedVersionIsRecognised) {
    ProducedLedger ledger;
    ledger.record(TicketId{"42"}, 5);
    EXPECT_TRUE(ledger.contains(TicketId{"42"}, 5));
}

TEST(ProducedLedger, UnrecordedVersionMisses) {
    ProducedLedger ledger;
    EXPECT_FALSE(ledger.contains(TicketId{"42"}, 5));
}

TEST(ProducedLedger, MatchesExactVersionOnlyNotANeighbour) {
    ProducedLedger ledger;
    ledger.record(TicketId{"42"}, 5);
    // A human edit lands at a strictly higher version and must pass through.
    EXPECT_FALSE(ledger.contains(TicketId{"42"}, 6));
    EXPECT_FALSE(ledger.contains(TicketId{"42"}, 4));
}

TEST(ProducedLedger, VersionsAreScopedPerTicketId) {
    ProducedLedger ledger;
    ledger.record(TicketId{"1"}, 3);
    EXPECT_TRUE(ledger.contains(TicketId{"1"}, 3));
    EXPECT_FALSE(ledger.contains(TicketId{"2"}, 3))
        << "version 3 of a different ticket is not ours";
}

TEST(ProducedLedger, MultipleVersionsOfOneTicketAllRecognised) {
    ProducedLedger ledger;
    ledger.record(TicketId{"7"}, 1);
    ledger.record(TicketId{"7"}, 2);
    ledger.record(TicketId{"7"}, 3);
    EXPECT_TRUE(ledger.contains(TicketId{"7"}, 1));
    EXPECT_TRUE(ledger.contains(TicketId{"7"}, 2));
    EXPECT_TRUE(ledger.contains(TicketId{"7"}, 3));
    EXPECT_FALSE(ledger.contains(TicketId{"7"}, 4));
}

TEST(ProducedLedger, ContainsIsRepeatableNotConsumed) {
    // A single produced version may be echoed by more than one webhook frame
    // (e.g. a created + updated pair); suppression must hold across both.
    ProducedLedger ledger;
    ledger.record(TicketId{"9"}, 2);
    EXPECT_TRUE(ledger.contains(TicketId{"9"}, 2));
    EXPECT_TRUE(ledger.contains(TicketId{"9"}, 2));
}

} // namespace
