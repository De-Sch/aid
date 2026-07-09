#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "aid/domain/CallTracker.h"
#include "aid/value-types/Ids.h"

namespace {

using aid::CallId;
using aid::domain::CallTracker;

CallId cid(std::string v) {
    return CallId{std::move(v)};
}

TEST(CallTrackerDecode, EmptyFieldReturnsEmpty) {
    EXPECT_TRUE(CallTracker::decode("").empty());
}

TEST(CallTrackerDecode, SingleId) {
    const auto ids = CallTracker::decode("X.1");
    ASSERT_EQ(ids.size(), 1u);
    EXPECT_EQ(ids[0], cid("X.1"));
}

TEST(CallTrackerDecode, ManyIdsCanonicalSeparator) {
    const auto ids = CallTracker::decode("X.1, X.2, X.3");
    ASSERT_EQ(ids.size(), 3u);
    EXPECT_EQ(ids[0], cid("X.1"));
    EXPECT_EQ(ids[1], cid("X.2"));
    EXPECT_EQ(ids[2], cid("X.3"));
}

TEST(CallTrackerDecode, ToleratesNoSpaceAfterComma) {
    const auto ids = CallTracker::decode("X.1,X.2,X.3");
    ASSERT_EQ(ids.size(), 3u);
    EXPECT_EQ(ids[0], cid("X.1"));
    EXPECT_EQ(ids[1], cid("X.2"));
    EXPECT_EQ(ids[2], cid("X.3"));
}

TEST(CallTrackerDecode, DropsEmptyPartFromDoubleComma) {
    const auto ids = CallTracker::decode("X.1, , X.2");
    ASSERT_EQ(ids.size(), 2u);
    EXPECT_EQ(ids[0], cid("X.1"));
    EXPECT_EQ(ids[1], cid("X.2"));
}

TEST(CallTrackerDecode, DropsTrailingSeparator) {
    // Trailing ', ' left over from older callers — decode trims and drops.
    const auto ids = CallTracker::decode("X.1, X.2, ");
    ASSERT_EQ(ids.size(), 2u);
    EXPECT_EQ(ids[0], cid("X.1"));
    EXPECT_EQ(ids[1], cid("X.2"));
}

TEST(CallTrackerDecode, TrimsStrayWhitespace) {
    const auto ids = CallTracker::decode("  X.1  ,   X.2\t");
    ASSERT_EQ(ids.size(), 2u);
    EXPECT_EQ(ids[0], cid("X.1"));
    EXPECT_EQ(ids[1], cid("X.2"));
}

TEST(CallTrackerEncode, EmptyListReturnsEmpty) {
    EXPECT_EQ(CallTracker::encode({}), "");
}

TEST(CallTrackerEncode, SingleIdNoSeparator) {
    const std::vector<CallId> ids{cid("X.1")};
    EXPECT_EQ(CallTracker::encode(ids), "X.1");
}

TEST(CallTrackerEncode, ManyIdsCommaSpace) {
    const std::vector<CallId> ids{cid("X.1"), cid("X.2"), cid("X.3")};
    EXPECT_EQ(CallTracker::encode(ids), "X.1, X.2, X.3");
}

TEST(CallTrackerContains, FindsPresent) {
    EXPECT_TRUE(CallTracker::contains("X.1, X.2, X.3", cid("X.2")));
}

TEST(CallTrackerContains, AbsentReturnsFalse) {
    EXPECT_FALSE(CallTracker::contains("X.1, X.2, X.3", cid("X.4")));
}

TEST(CallTrackerContains, EmptyFieldAbsent) {
    EXPECT_FALSE(CallTracker::contains("", cid("X.1")));
}

TEST(CallTrackerWithAdded, AppendsToNonEmpty) {
    EXPECT_EQ(CallTracker::withAdded("X.1, X.2", cid("X.3")), "X.1, X.2, X.3");
}

TEST(CallTrackerWithAdded, IdempotentOnExisting) {
    EXPECT_EQ(CallTracker::withAdded("X.1, X.2", cid("X.1")), "X.1, X.2");
}

TEST(CallTrackerWithAdded, ToEmptyFieldReturnsBareId) {
    EXPECT_EQ(CallTracker::withAdded("", cid("X.1")), "X.1");
}

TEST(CallTrackerWithRemoved, RemovesMiddle) {
    EXPECT_EQ(CallTracker::withRemoved("X.1, X.2, X.3", cid("X.2")), "X.1, X.3");
}

TEST(CallTrackerWithRemoved, RemovesFirst) {
    EXPECT_EQ(CallTracker::withRemoved("X.1, X.2, X.3", cid("X.1")), "X.2, X.3");
}

TEST(CallTrackerWithRemoved, RemovesLast) {
    EXPECT_EQ(CallTracker::withRemoved("X.1, X.2, X.3", cid("X.3")), "X.1, X.2");
}

TEST(CallTrackerWithRemoved, AbsentIsNoop) {
    // Removing a non-present id — withRemoved returns the field unchanged.
    // (Modulo decode/encode normalisation, which is acceptable.)
    EXPECT_EQ(CallTracker::withRemoved("X.1, X.2, X.3", cid("X.4")), "X.1, X.2, X.3");
}

TEST(CallTrackerWithRemoved, SoleIdYieldsEmpty) {
    EXPECT_EQ(CallTracker::withRemoved("X.1", cid("X.1")), "");
}

TEST(CallTrackerWithRemoved, NormalisesTrailingSeparator) {
    // Composes decode (which drops trailing ", ") with encode (canonical).
    EXPECT_EQ(CallTracker::withRemoved("X.1, X.2, ", cid("X.2")), "X.1");
}

} // namespace
