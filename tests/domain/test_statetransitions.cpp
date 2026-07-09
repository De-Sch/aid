#include <gtest/gtest.h>

#include <vector>

#include "aid/domain/StateTransitions.h"
#include "aid/value-types/Ids.h"

namespace {

using aid::TicketStatus;
using aid::domain::StateTransitions;

TEST(StateTransitionsPath, NewToInProgressIsDirect) {
    EXPECT_EQ(StateTransitions::path(TicketStatus::New, TicketStatus::InProgress),
              std::vector<TicketStatus>{TicketStatus::InProgress});
    EXPECT_TRUE(StateTransitions::canTransitionDirect(TicketStatus::New, TicketStatus::InProgress));
}

TEST(StateTransitionsPath, NewToClosedRoutesThroughInProgress) {
    // OpenProject's workflow forbids direct New -> Closed; the path runs
    // through InProgress. Vector ordering matters: the adapter PATCHes in
    // order.
    const std::vector<TicketStatus> expected{TicketStatus::InProgress, TicketStatus::Closed};
    EXPECT_EQ(StateTransitions::path(TicketStatus::New, TicketStatus::Closed), expected);
    EXPECT_FALSE(StateTransitions::canTransitionDirect(TicketStatus::New, TicketStatus::Closed));
}

TEST(StateTransitionsPath, InProgressToClosedIsDirect) {
    EXPECT_EQ(StateTransitions::path(TicketStatus::InProgress, TicketStatus::Closed),
              std::vector<TicketStatus>{TicketStatus::Closed});
    EXPECT_TRUE(
        StateTransitions::canTransitionDirect(TicketStatus::InProgress, TicketStatus::Closed));
}

TEST(StateTransitionsPath, InProgressToNewIsForbidden) {
    EXPECT_TRUE(StateTransitions::path(TicketStatus::InProgress, TicketStatus::New).empty());
    EXPECT_FALSE(
        StateTransitions::canTransitionDirect(TicketStatus::InProgress, TicketStatus::New));
}

TEST(StateTransitionsPath, ClosedIsTerminal) {
    for (auto to : {TicketStatus::New, TicketStatus::InProgress, TicketStatus::Closed}) {
        EXPECT_TRUE(StateTransitions::path(TicketStatus::Closed, to).empty())
            << "Closed -> " << static_cast<int>(to) << " should be empty";
        EXPECT_FALSE(StateTransitions::canTransitionDirect(TicketStatus::Closed, to));
    }
}

TEST(StateTransitionsPath, SelfTransitionsReturnEmpty) {
    // Steps for Closed: [] -> return immediately, idempotent.
    // Same idempotence extends to other self-transitions.
    EXPECT_TRUE(StateTransitions::path(TicketStatus::New, TicketStatus::New).empty());
    EXPECT_TRUE(StateTransitions::path(TicketStatus::InProgress, TicketStatus::InProgress).empty());
    EXPECT_TRUE(StateTransitions::path(TicketStatus::Closed, TicketStatus::Closed).empty());
}

TEST(StateTransitionsCanDirect, NewToClosedIsNotDirect) {
    // Even though path is non-empty for New -> Closed, it requires two
    // PATCHes — canTransitionDirect must say so.
    EXPECT_FALSE(StateTransitions::canTransitionDirect(TicketStatus::New, TicketStatus::Closed));
}

} // namespace
