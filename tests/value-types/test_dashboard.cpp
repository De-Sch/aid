#include <gtest/gtest.h>

#include "aid/value-types/Dashboard.h"

TEST(DashboardEntry, DefaultsAreSafe) {
    aid::DashboardEntry e;
    EXPECT_TRUE(e.id.empty());
    EXPECT_TRUE(e.subject.empty());
    EXPECT_EQ(e.status, aid::TicketStatus::New);
    EXPECT_TRUE(e.statusId.empty());
    EXPECT_TRUE(e.callIds.empty());
    EXPECT_TRUE(e.callerNumber.empty());
    EXPECT_FALSE(e.calledNumber.has_value());
    EXPECT_FALSE(e.assignee.has_value());
    EXPECT_TRUE(e.href.empty());
    EXPECT_TRUE(e.projectName.empty());
    EXPECT_FALSE(e.activeCallForViewer.has_value());
    EXPECT_TRUE(e.description.empty());
}

TEST(DashboardEntry, ActiveCallForViewerIsOptional) {
    aid::DashboardEntry e;
    e.activeCallForViewer = aid::CallId{"42"};
    ASSERT_TRUE(e.activeCallForViewer.has_value());
    EXPECT_EQ(e.activeCallForViewer->v, "42");
}

TEST(ActiveCall, AggregateInitialises) {
    aid::ActiveCall a{
        .ticketId = aid::TicketId{"t1"},
        .callId = aid::CallId{"c1"},
        .projectName = "Acme",
        .callerNumber = aid::PhoneNumber{"+491"},
    };
    EXPECT_EQ(a.ticketId.v, "t1");
    EXPECT_EQ(a.callId.v, "c1");
    EXPECT_EQ(a.projectName, "Acme");
    EXPECT_EQ(a.callerNumber.v, "+491");
}

TEST(DashboardView, DefaultsToEmpty) {
    aid::DashboardView v;
    EXPECT_TRUE(v.tickets.empty());
    EXPECT_FALSE(v.active.has_value());
    EXPECT_FALSE(v.addressCallInformation.has_value());
}

TEST(DashboardView, HoldsTicketsAndActiveCallAndAddressInfo) {
    aid::DashboardEntry e1;
    e1.id = aid::TicketId{"t"};
    aid::DashboardEntry e2;
    e2.id = aid::TicketId{"u"};
    aid::ActiveCall ac;
    ac.ticketId = aid::TicketId{"t"};

    aid::Contact contact;
    contact.name = "Bob";
    contact.companyName = "ACME";
    contact.kind = aid::AddressKind::Person;

    aid::DashboardView v;
    v.tickets.push_back(e1);
    v.tickets.push_back(e2);
    v.active = ac;
    v.addressCallInformation = contact;

    ASSERT_EQ(v.tickets.size(), 2u);
    EXPECT_EQ(v.tickets[0].id.v, "t");
    ASSERT_TRUE(v.active.has_value());
    EXPECT_EQ(v.active->ticketId.v, "t");
    ASSERT_TRUE(v.addressCallInformation.has_value());
    EXPECT_EQ(v.addressCallInformation->name, "Bob");
    EXPECT_EQ(v.addressCallInformation->companyName, "ACME");
    EXPECT_EQ(v.addressCallInformation->kind, aid::AddressKind::Person);
}
