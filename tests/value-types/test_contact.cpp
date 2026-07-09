#include <gtest/gtest.h>

#include "aid/value-types/Contact.h"

TEST(Contact, DefaultsToPerson) {
    aid::Contact c;
    EXPECT_EQ(c.kind, aid::AddressKind::Person);
    EXPECT_TRUE(c.name.empty());
    EXPECT_TRUE(c.companyName.empty());
    EXPECT_TRUE(c.phoneNumbers.empty());
    EXPECT_TRUE(c.projectIds.empty());
}

TEST(Contact, AggregateInitialisesAllFields) {
    aid::Contact c{
        .name = "Alice",
        .companyName = "Acme",
        .kind = aid::AddressKind::Company,
        .phoneNumbers = {aid::PhoneNumber{"+491"}, aid::PhoneNumber{"+492"}},
        .projectIds = {aid::ProjectId{"42"}},
    };
    EXPECT_EQ(c.name, "Alice");
    EXPECT_EQ(c.companyName, "Acme");
    EXPECT_EQ(c.kind, aid::AddressKind::Company);
    ASSERT_EQ(c.phoneNumbers.size(), 2u);
    EXPECT_EQ(c.phoneNumbers[0].v, "+491");
    EXPECT_EQ(c.phoneNumbers[1].v, "+492");
    ASSERT_EQ(c.projectIds.size(), 1u);
    EXPECT_EQ(c.projectIds[0].v, "42");
}

TEST(Contact, EmptyProjectIdsRepresentsUnknownCaller) {
    aid::Contact c;
    c.name = "Bob";
    EXPECT_TRUE(c.projectIds.empty());
    EXPECT_EQ(c.kind, aid::AddressKind::Person);
}

TEST(AddressKind, PersonAndCompanyAreDistinct) {
    EXPECT_NE(aid::AddressKind::Person, aid::AddressKind::Company);
}
