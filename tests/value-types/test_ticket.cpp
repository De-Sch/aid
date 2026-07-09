#include <gtest/gtest.h>

#include "aid/value-types/Ticket.h"

TEST(Ticket, DefaultsAreSafe) {
    aid::Ticket t;
    EXPECT_TRUE(t.id.empty());
    EXPECT_TRUE(t.projectId.empty());
    EXPECT_TRUE(t.subject.empty());
    EXPECT_EQ(t.status, aid::TicketStatus::New);
    EXPECT_FALSE(t.assignee.has_value());
    EXPECT_TRUE(t.callIds.empty());
    EXPECT_TRUE(t.callerNumber.empty());
    EXPECT_FALSE(t.calledNumber.has_value());
    EXPECT_FALSE(t.callStart.has_value());
    EXPECT_FALSE(t.callEnd.has_value());
    EXPECT_TRUE(t.description.empty());
    EXPECT_EQ(t.lockVersion, 0);
}

TEST(Ticket, EmptyCallIdsIsValidOpenTicket) {
    aid::Ticket t;
    t.status = aid::TicketStatus::InProgress;
    EXPECT_TRUE(t.callIds.empty());
    EXPECT_EQ(t.status, aid::TicketStatus::InProgress);
}

TEST(Ticket, HoldsMultipleConcurrentCallIds) {
    aid::Ticket t;
    t.callIds = {aid::CallId{"a"}, aid::CallId{"b"}, aid::CallId{"c"}};
    ASSERT_EQ(t.callIds.size(), 3u);
    EXPECT_EQ(t.callIds[0].v, "a");
    EXPECT_EQ(t.callIds[2].v, "c");
}

TEST(Ticket, LockVersionRoundTrips) {
    aid::Ticket t;
    t.lockVersion = 7;
    EXPECT_EQ(t.lockVersion, 7);
}

TEST(NewTicket, DefaultsToNewStatusAndEmptyOptionals) {
    aid::NewTicket nt;
    EXPECT_EQ(nt.status, aid::TicketStatus::New);
    EXPECT_FALSE(nt.calledNumber.has_value());
    EXPECT_FALSE(nt.assignee.has_value());
}

TEST(NewTicket, IncomingShapeHasCalledNumberNoAssignee) {
    aid::NewTicket nt;
    nt.projectId = aid::ProjectId{"p"};
    nt.subject = "Caller Name";
    nt.callId = aid::CallId{"c1"};
    nt.callerNumber = aid::PhoneNumber{"+491"};
    nt.calledNumber = aid::PhoneNumber{"+490"};
    EXPECT_TRUE(nt.calledNumber.has_value());
    EXPECT_FALSE(nt.assignee.has_value());
}

TEST(NewTicket, OutgoingShapeHasAssigneeNoCalledNumber) {
    aid::NewTicket nt;
    nt.projectId = aid::ProjectId{"p"};
    nt.subject = "Caller Name";
    nt.callId = aid::CallId{"c2"};
    nt.callerNumber = aid::PhoneNumber{"+491"};
    nt.assignee = aid::UserHandle{"alice"};
    EXPECT_FALSE(nt.calledNumber.has_value());
    ASSERT_TRUE(nt.assignee.has_value());
    EXPECT_EQ(nt.assignee->v, "alice");
}
