#include <gtest/gtest.h>

#include <chrono>
#include <unordered_map>
#include <unordered_set>

#include "aid/value-types/Ids.h"

TEST(Id, DefaultConstructedIsEmpty) {
    aid::CallId id;
    EXPECT_TRUE(id.empty());
    EXPECT_TRUE(id.v.empty());
}

TEST(Id, NonEmptyValueIsNotEmpty) {
    aid::CallId id{"abc"};
    EXPECT_FALSE(id.empty());
    EXPECT_EQ(id.v, "abc");
}

TEST(Id, EqualityComparesValues) {
    aid::CallId a{"1"};
    aid::CallId b{"1"};
    aid::CallId c{"2"};
    EXPECT_EQ(a, b);
    EXPECT_NE(a, c);
}

TEST(Id, OrderingComparesValues) {
    aid::TicketId a{"100"};
    aid::TicketId b{"200"};
    aid::TicketId aa{"100"};
    EXPECT_LT(a, b);
    EXPECT_FALSE(b < a);
    EXPECT_FALSE(a < aa);
}

TEST(Id, IsUsableInUnorderedSet) {
    std::unordered_set<aid::CallId> s;
    s.insert(aid::CallId{"a"});
    s.insert(aid::CallId{"b"});
    s.insert(aid::CallId{"a"});
    EXPECT_EQ(s.size(), 2u);
    EXPECT_EQ(s.count(aid::CallId{"a"}), 1u);
    EXPECT_EQ(s.count(aid::CallId{"c"}), 0u);
}

TEST(Id, IsUsableInUnorderedMap) {
    std::unordered_map<aid::TicketId, int> m;
    m[aid::TicketId{"42"}] = 1;
    m[aid::TicketId{"99"}] = 2;
    EXPECT_EQ(m[aid::TicketId{"42"}], 1);
    EXPECT_EQ(m[aid::TicketId{"99"}], 2);
}

TEST(PhoneNumber, EmptyByDefault) {
    aid::PhoneNumber p;
    EXPECT_TRUE(p.empty());
}

TEST(PhoneNumber, EqualityAndOrdering) {
    aid::PhoneNumber a{"+491234"};
    aid::PhoneNumber b{"+491234"};
    aid::PhoneNumber c{"+491235"};
    EXPECT_EQ(a, b);
    EXPECT_NE(a, c);
    EXPECT_LT(a, c);
}

TEST(PhoneNumber, IsHashable) {
    std::unordered_set<aid::PhoneNumber> s;
    s.insert(aid::PhoneNumber{"+491234"});
    s.insert(aid::PhoneNumber{"+491234"});
    s.insert(aid::PhoneNumber{"+491235"});
    EXPECT_EQ(s.size(), 2u);
}

TEST(TicketStatus, EnumeratorsAreDistinct) {
    EXPECT_NE(aid::TicketStatus::New, aid::TicketStatus::InProgress);
    EXPECT_NE(aid::TicketStatus::InProgress, aid::TicketStatus::Closed);
    EXPECT_NE(aid::TicketStatus::Closed, aid::TicketStatus::New);
}

TEST(Timestamp, IsSystemClockTimePoint) {
    aid::Timestamp now = std::chrono::system_clock::now();
    aid::Timestamp later = now + std::chrono::seconds(1);
    EXPECT_LT(now, later);
}
