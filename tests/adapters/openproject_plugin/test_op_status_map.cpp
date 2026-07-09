#include <gtest/gtest.h>

#include "aid/adapters/openproject/internal/OpStatusMap.h"
#include "aid/crosscutting/Config.h"
#include "aid/value-types/Ids.h"

using aid::TicketStatus;
using aid::adapters::openproject::OpStatusMap;
using aid::crosscutting::TicketSystemConfig;

namespace {

TicketSystemConfig sampleCfg() {
    TicketSystemConfig cfg;
    cfg.statusNew = aid::StatusId{"1"};
    cfg.statusInProgress = aid::StatusId{"2"};
    cfg.statusClosed = aid::StatusId{"3"};
    return cfg;
}

} // namespace

TEST(OpStatusMap, ForwardLookupReturnsConfiguredStatusId) {
    const auto m = OpStatusMap::fromConfig(sampleCfg());
    EXPECT_EQ(m.hrefIdFor(TicketStatus::New).v, "1");
    EXPECT_EQ(m.hrefIdFor(TicketStatus::InProgress).v, "2");
    EXPECT_EQ(m.hrefIdFor(TicketStatus::Closed).v, "3");
}

TEST(OpStatusMap, ReverseLookupReturnsConfiguredEnum) {
    const auto m = OpStatusMap::fromConfig(sampleCfg());
    EXPECT_EQ(m.statusFor(aid::StatusId{"1"}), TicketStatus::New);
    EXPECT_EQ(m.statusFor(aid::StatusId{"2"}), TicketStatus::InProgress);
    EXPECT_EQ(m.statusFor(aid::StatusId{"3"}), TicketStatus::Closed);
}

TEST(OpStatusMap, RoundTripsForEveryStatus) {
    const auto m = OpStatusMap::fromConfig(sampleCfg());
    for (auto s : {TicketStatus::New, TicketStatus::InProgress, TicketStatus::Closed}) {
        EXPECT_EQ(m.statusFor(m.hrefIdFor(s)), s);
    }
}

TEST(OpStatusMap, ReverseLookupForUnknownIdDefaultsToNew) {
    const auto m = OpStatusMap::fromConfig(sampleCfg());
    // Unknown id — never configured. The defensive branch returns New
    // and emits a warning to the logger. We don't capture the log here;
    // the contract is "do not crash, do not throw".
    EXPECT_EQ(m.statusFor(aid::StatusId{"99"}), TicketStatus::New);
}

TEST(OpStatusMap, ReverseLookupForEmptyIdDefaultsToNew) {
    const auto m = OpStatusMap::fromConfig(sampleCfg());
    EXPECT_EQ(m.statusFor(aid::StatusId{""}), TicketStatus::New);
}

TEST(OpStatusMap, HrefIdForOnUnconfiguredStatusReturnsEmptyAndDoesNotThrow) {
    // Empty config — every forward_ slot is the default StatusId{""}.
    // hrefIdFor must NOT throw across the plugin .so boundary; instead
    // it logs + returns the empty StatusId. The downstream URL will be
    // "/api/v3/statuses/" which OpenProject 4xx's, surfacing as a normal
    // Error in the adapter chain.
    TicketSystemConfig empty;
    const auto m = OpStatusMap::fromConfig(empty);
    aid::StatusId out;
    EXPECT_NO_THROW(out = m.hrefIdFor(TicketStatus::Closed));
    EXPECT_TRUE(out.v.empty());
}
