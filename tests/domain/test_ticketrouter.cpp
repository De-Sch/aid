#include <gtest/gtest.h>

#include <array>
#include <optional>
#include <span>
#include <variant>

#include "aid/domain/TicketRouter.h"
#include "aid/value-types/Ids.h"
#include "aid/value-types/Ticket.h"

namespace {

using aid::ProjectId;
using aid::Ticket;
using aid::TicketId;
using aid::domain::TicketRouter;

Ticket make_ticket(TicketId id, ProjectId pid) {
    Ticket t;
    t.id = std::move(id);
    t.projectId = std::move(pid);
    return t;
}

TEST(TicketRouterDecideKnown, FirstProjectHasCandidateReuses) {
    const auto p1 = ProjectId{"P1"};
    const auto p2 = ProjectId{"P2"};
    const std::array cands{
        TicketRouter::RoutingCandidate{p1, make_ticket(TicketId{"T1"}, p1)},
        TicketRouter::RoutingCandidate{p2, std::nullopt},
    };

    const auto decision = TicketRouter::decideKnown(TicketRouter::KnownInput{std::span{cands}});

    ASSERT_TRUE(std::holds_alternative<TicketRouter::ReuseExisting>(decision));
    EXPECT_EQ(std::get<TicketRouter::ReuseExisting>(decision).ticket, TicketId{"T1"});
}

TEST(TicketRouterDecideKnown, SecondProjectHasCandidateReusesItNotFallback) {
    const auto p1 = ProjectId{"P1"};
    const auto p2 = ProjectId{"P2"};
    const std::array cands{
        TicketRouter::RoutingCandidate{p1, std::nullopt},
        TicketRouter::RoutingCandidate{p2, make_ticket(TicketId{"T2"}, p2)},
    };

    const auto decision = TicketRouter::decideKnown(TicketRouter::KnownInput{std::span{cands}});

    ASSERT_TRUE(std::holds_alternative<TicketRouter::ReuseExisting>(decision));
    EXPECT_EQ(std::get<TicketRouter::ReuseExisting>(decision).ticket, TicketId{"T2"});
}

TEST(TicketRouterDecideKnown, AllEmptyFallsBackToFirstProject) {
    const auto p1 = ProjectId{"P1"};
    const auto p2 = ProjectId{"P2"};
    const auto p3 = ProjectId{"P3"};
    const std::array cands{
        TicketRouter::RoutingCandidate{p1, std::nullopt},
        TicketRouter::RoutingCandidate{p2, std::nullopt},
        TicketRouter::RoutingCandidate{p3, std::nullopt},
    };

    const auto decision = TicketRouter::decideKnown(TicketRouter::KnownInput{std::span{cands}});

    ASSERT_TRUE(std::holds_alternative<TicketRouter::CreateInProject>(decision));
    EXPECT_EQ(std::get<TicketRouter::CreateInProject>(decision).project, p1);
}

TEST(TicketRouterDecideKnown, ThirdProjectHasCandidateWins) {
    const auto p1 = ProjectId{"P1"};
    const auto p2 = ProjectId{"P2"};
    const auto p3 = ProjectId{"P3"};
    const std::array cands{
        TicketRouter::RoutingCandidate{p1, std::nullopt},
        TicketRouter::RoutingCandidate{p2, std::nullopt},
        TicketRouter::RoutingCandidate{p3, make_ticket(TicketId{"T3"}, p3)},
    };

    const auto decision = TicketRouter::decideKnown(TicketRouter::KnownInput{std::span{cands}});

    ASSERT_TRUE(std::holds_alternative<TicketRouter::ReuseExisting>(decision));
    EXPECT_EQ(std::get<TicketRouter::ReuseExisting>(decision).ticket, TicketId{"T3"});
}

TEST(TicketRouterDecideUnknown, ByNameBeatsByNumber) {
    TicketRouter::UnknownInput in{
        .byName = make_ticket(TicketId{"TN"}, ProjectId{"PN"}),
        .byNumber = make_ticket(TicketId{"TP"}, ProjectId{"PP"}),
        .unknownFallback = ProjectId{"FB"},
    };

    const auto decision = TicketRouter::decideUnknown(in);

    ASSERT_TRUE(std::holds_alternative<TicketRouter::ReuseExisting>(decision));
    EXPECT_EQ(std::get<TicketRouter::ReuseExisting>(decision).ticket, TicketId{"TN"});
}

TEST(TicketRouterDecideUnknown, ByNumberUsedWhenByNameAbsent) {
    TicketRouter::UnknownInput in{
        .byName = std::nullopt,
        .byNumber = make_ticket(TicketId{"TP"}, ProjectId{"PP"}),
        .unknownFallback = ProjectId{"FB"},
    };

    const auto decision = TicketRouter::decideUnknown(in);

    ASSERT_TRUE(std::holds_alternative<TicketRouter::ReuseExisting>(decision));
    EXPECT_EQ(std::get<TicketRouter::ReuseExisting>(decision).ticket, TicketId{"TP"});
}

TEST(TicketRouterDecideUnknown, BothAbsentCreatesInFallback) {
    TicketRouter::UnknownInput in{
        .byName = std::nullopt,
        .byNumber = std::nullopt,
        .unknownFallback = ProjectId{"FB"},
    };

    const auto decision = TicketRouter::decideUnknown(in);

    ASSERT_TRUE(std::holds_alternative<TicketRouter::CreateInProject>(decision));
    EXPECT_EQ(std::get<TicketRouter::CreateInProject>(decision).project, ProjectId{"FB"});
}

TEST(TicketRouterDecideKnown, SingleProjectNoCandidateCreatesThere) {
    const auto p1 = ProjectId{"P1"};
    const std::array cands{
        TicketRouter::RoutingCandidate{p1, std::nullopt},
    };

    const auto decision = TicketRouter::decideKnown(TicketRouter::KnownInput{std::span{cands}});

    ASSERT_TRUE(std::holds_alternative<TicketRouter::CreateInProject>(decision));
    EXPECT_EQ(std::get<TicketRouter::CreateInProject>(decision).project, p1);
}

} // namespace
