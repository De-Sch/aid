#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <optional>
#include <string>

#include "aid/domain/CallLineFormatter.h"
#include "aid/value-types/Ids.h"

namespace {

using aid::CallId;
using aid::Timestamp;
using aid::UserHandle;
using aid::domain::CallLineFormatter;

class CallLineFormatterTest : public ::testing::Test {
protected:
    // The daemon is pinned to Europe/Berlin. Mirror it so timestamp
    // round-trips are deterministic across hosts.
    void SetUp() override {
        ::setenv("TZ", "Europe/Berlin", 1);
        ::tzset();
    }
};

Timestamp mkLocal(int y, int mo, int d, int h, int mi, int s) {
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = mi;
    tm.tm_sec = s;
    tm.tm_isdst = -1;
    const auto t = std::mktime(&tm);
    return std::chrono::system_clock::from_time_t(t);
}

// --- buildStart -------------------------------------------------------

TEST_F(CallLineFormatterTest, BuildStartProducesCanonicalLine) {
    const auto out = CallLineFormatter::buildStart(UserHandle{"alice"},
                                                   mkLocal(2026, 5, 20, 14, 0, 0), CallId{"X.1"});
    EXPECT_EQ(out, "alice: Call start: 2026-05-20 14:00:00 (X.1)");
}

// --- findLineFor ------------------------------------------------------

TEST(CallLineFormatterFindLineFor, FirstLine) {
    const std::string desc = "alice: Call start: 2026-05-20 14:00:00 (X.1)\nbob: x";
    const auto span = CallLineFormatter::findLineFor(desc, CallId{"X.1"});
    ASSERT_TRUE(span.has_value());
    EXPECT_EQ(desc.substr(span->begin, span->end - span->begin),
              "alice: Call start: 2026-05-20 14:00:00 (X.1)");
}

TEST(CallLineFormatterFindLineFor, MiddleLine) {
    const std::string desc = "header\n"
                             "alice: Call start: 2026-05-20 14:00:00 (X.2)\n"
                             "footer";
    const auto span = CallLineFormatter::findLineFor(desc, CallId{"X.2"});
    ASSERT_TRUE(span.has_value());
    EXPECT_EQ(desc.substr(span->begin, span->end - span->begin),
              "alice: Call start: 2026-05-20 14:00:00 (X.2)");
}

TEST(CallLineFormatterFindLineFor, LastLineNoTrailingNewline) {
    const std::string desc = "header\nalice: Call start: 2026-05-20 14:00:00 (X.3)";
    const auto span = CallLineFormatter::findLineFor(desc, CallId{"X.3"});
    ASSERT_TRUE(span.has_value());
    EXPECT_EQ(desc.substr(span->begin, span->end - span->begin),
              "alice: Call start: 2026-05-20 14:00:00 (X.3)");
}

TEST(CallLineFormatterFindLineFor, AbsentCallIdYieldsNullopt) {
    EXPECT_FALSE(
        CallLineFormatter::findLineFor("alice: Call start: ... (X.1)", CallId{"Z.9"}).has_value());
}

// --- rewriteUser ------------------------------------------------------

TEST(CallLineFormatterRewriteUser, ReplacesPrefix) {
    const auto out = CallLineFormatter::rewriteUser("alice: Call start: 2026-05-20 14:00:00 (X.1)",
                                                    UserHandle{"bob"});
    EXPECT_EQ(out, "bob: Call start: 2026-05-20 14:00:00 (X.1)");
}

TEST(CallLineFormatterRewriteUser, NoColonReturnsInputUnchanged) {
    // Graceful — don't crash a broken line.
    EXPECT_EQ(CallLineFormatter::rewriteUser("no colon here", UserHandle{"bob"}), "no colon here");
}

// --- complete ---------------------------------------------------------

TEST_F(CallLineFormatterTest, CompleteStripsCallidAndStampsEnd) {
    // The callid is dropped on completion (it only existed to locate the open
    // line); the completed line carries just the end-time marker.
    const auto out = CallLineFormatter::complete("alice: Call start: 2026-05-20 14:00:00 (X.1)",
                                                 mkLocal(2026, 5, 20, 14, 23, 0));
    EXPECT_EQ(out, "alice: Call start: 2026-05-20 14:00:00 Call End: 2026-05-20 14:23:00");
}

TEST_F(CallLineFormatterTest, CompletedLineIsNoLongerFoundByCallid) {
    // Safety + idempotency: once completed, the line can't be re-located by
    // callid (the callid is gone), so a replayed hangup can't double-complete it.
    const auto completed = CallLineFormatter::complete(
        "alice: Call start: 2026-05-20 14:00:00 (X.1)", mkLocal(2026, 5, 20, 14, 23, 0));
    EXPECT_EQ(completed.find("(X.1)"), std::string::npos);
    EXPECT_FALSE(CallLineFormatter::findLineFor(completed, CallId{"X.1"}).has_value());
}

// --- parseStart -------------------------------------------------------

TEST_F(CallLineFormatterTest, ParseStartRoundTrip) {
    const auto ts = mkLocal(2026, 5, 20, 14, 0, 0);
    const auto line = CallLineFormatter::buildStart(UserHandle{"alice"}, ts, CallId{"X.1"});
    const auto parsed = CallLineFormatter::parseStart(line);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->user, UserHandle{"alice"});
    EXPECT_EQ(parsed->start, ts);
}

TEST(CallLineFormatterParseStart, MissingColonReturnsNullopt) {
    EXPECT_FALSE(CallLineFormatter::parseStart("alice no colon").has_value());
}

TEST(CallLineFormatterParseStart, MissingCallStartMarkerReturnsNullopt) {
    EXPECT_FALSE(CallLineFormatter::parseStart("alice: hi there (X.1)").has_value());
}

TEST(CallLineFormatterParseStart, MissingOpenParenReturnsNullopt) {
    EXPECT_FALSE(
        CallLineFormatter::parseStart("alice: Call start: 2026-05-20 14:00:00").has_value());
}

TEST_F(CallLineFormatterTest, ParseStartUnparseableTimestampReturnsNullopt) {
    EXPECT_FALSE(CallLineFormatter::parseStart("alice: Call start: not-a-time (X.1)").has_value());
}

// --- findOpenLineForUser ----------------------------------------------

TEST(CallLineFormatterFindOpenLineForUser, ReturnsFirstOpenCallId) {
    // Supersedes the old "most recent line wins" rfind: when a user has two open
    // lines, the FIRST open one wins. Scanning forward keeps
    // this in lock-step with findUsersWithOpenCalls' first-seen order.
    const std::string desc = "alice: Call start: 2026-05-20 14:00:00 (X.1)\n"
                             "alice: Call start: 2026-05-20 14:05:00 (X.2)\n";
    const auto cid = CallLineFormatter::findOpenLineForUser(desc, UserHandle{"alice"});
    ASSERT_TRUE(cid.has_value());
    EXPECT_EQ(*cid, CallId{"X.1"});
}

TEST(CallLineFormatterFindOpenLineForUser, EarlierOpenLaterClosedReportsTheOpenCall) {
    // The bug this fixes: an earlier still-open call followed by
    // a later already-hung-up call on the same ticket. The old rfind landed on the
    // closed line and wrongly reported "no active call." We must surface the
    // open call's callid (X.1), not nullopt.
    const std::string desc = "alice: Call start: 2026-05-20 14:00:00 (X.1)\n"
                             "alice: Call start: 2026-05-20 14:05:00 Call End: "
                             "2026-05-20 14:10:00\n";
    const auto cid = CallLineFormatter::findOpenLineForUser(desc, UserHandle{"alice"});
    ASSERT_TRUE(cid.has_value());
    EXPECT_EQ(*cid, CallId{"X.1"});
}

TEST(CallLineFormatterFindOpenLineForUser, AgreesWithFindUsersWithOpenCallsOnMultiCallTicket) {
    // The two detectors must agree on WHETHER a user holds an open call, for any
    // multi-line arrangement.
    const std::string desc = "alice: Call start: 2026-05-20 14:00:00 (X.1)\n"
                             "alice: Call start: 2026-05-20 14:05:00 Call End: "
                             "2026-05-20 14:10:00\n"
                             "bob: Call start: 2026-05-20 14:12:00 Call End: "
                             "2026-05-20 14:15:00\n";
    const auto users = CallLineFormatter::findUsersWithOpenCalls(desc);
    // alice is open, bob is fully closed.
    ASSERT_EQ(users.size(), 1U);
    EXPECT_EQ(users[0], UserHandle{"alice"});
    EXPECT_TRUE(CallLineFormatter::findOpenLineForUser(desc, UserHandle{"alice"}).has_value());
    EXPECT_FALSE(CallLineFormatter::findOpenLineForUser(desc, UserHandle{"bob"}).has_value());
}

TEST(CallLineFormatterFindOpenLineForUser, SkipsClosedLine) {
    const std::string desc = "alice: Call start: 2026-05-20 14:00:00 Call End: "
                             "2026-05-20 14:10:00\n";
    EXPECT_FALSE(CallLineFormatter::findOpenLineForUser(desc, UserHandle{"alice"}).has_value());
}

TEST(CallLineFormatterFindOpenLineForUser, MalformedParensYieldsNullopt) {
    // ')' before '(' on the matched line → bail rather than return garbage.
    const std::string desc = "alice: Call start: )broken(";
    EXPECT_FALSE(CallLineFormatter::findOpenLineForUser(desc, UserHandle{"alice"}).has_value());
}

TEST(CallLineFormatterFindOpenLineForUser, AbsentUserYieldsNullopt) {
    const std::string desc = "alice: Call start: 2026-05-20 14:00:00 (X.1)";
    EXPECT_FALSE(CallLineFormatter::findOpenLineForUser(desc, UserHandle{"bob"}).has_value());
}

// --- hasLine ----------------------------------------------------------

TEST(CallLineFormatterHasLine, FindsMatchingUserAndCallId) {
    const std::string desc = "alice: Call start: 2026-05-20 14:00:00 (X.1)";
    EXPECT_TRUE(CallLineFormatter::hasLine(desc, UserHandle{"alice"}, CallId{"X.1"}));
}

TEST(CallLineFormatterHasLine, SameUserDifferentCallIdReturnsFalse) {
    const std::string desc = "alice: Call start: 2026-05-20 14:00:00 (X.1)";
    EXPECT_FALSE(CallLineFormatter::hasLine(desc, UserHandle{"alice"}, CallId{"X.9"}));
}

TEST(CallLineFormatterHasLine, CorrectCallIdWrongUserReturnsFalse) {
    const std::string desc = "alice: Call start: 2026-05-20 14:00:00 (X.1)";
    EXPECT_FALSE(CallLineFormatter::hasLine(desc, UserHandle{"bob"}, CallId{"X.1"}));
}

TEST(CallLineFormatterHasLine, FindsMatchAmongMultipleSameUserLines) {
    // Dedup test must work across multiple lines for the same user.
    const std::string desc = "alice: Call start: 2026-05-20 14:00:00 (X.1)\n"
                             "alice: Call start: 2026-05-20 14:05:00 (X.2)\n";
    EXPECT_TRUE(CallLineFormatter::hasLine(desc, UserHandle{"alice"}, CallId{"X.2"}));
}

TEST(CallLineFormatterHasLine, EmptyDescriptionIsFalse) {
    EXPECT_FALSE(CallLineFormatter::hasLine("", UserHandle{"alice"}, CallId{"X.1"}));
}

// --- findUsersWithOpenCalls -------------------------------------------

TEST(CallLineFormatterFindUsersWithOpenCalls, EmptyDescriptionYieldsNone) {
    EXPECT_TRUE(CallLineFormatter::findUsersWithOpenCalls("").empty());
}

TEST(CallLineFormatterFindUsersWithOpenCalls, SingleOpenLineYieldsItsUser) {
    const std::string desc = "alice: Call start: 2026-05-20 14:00:00 (X.1)";
    const auto users = CallLineFormatter::findUsersWithOpenCalls(desc);
    ASSERT_EQ(users.size(), 1U);
    EXPECT_EQ(users[0], UserHandle{"alice"});
}

TEST(CallLineFormatterFindUsersWithOpenCalls, ClosedLineIsExcluded) {
    const std::string desc = "alice: Call start: 2026-05-20 14:00:00 Call End: "
                             "2026-05-20 14:10:00\n";
    EXPECT_TRUE(CallLineFormatter::findUsersWithOpenCalls(desc).empty());
}

TEST(CallLineFormatterFindUsersWithOpenCalls, OpenAndClosedMixKeepsOnlyOpen) {
    // alice's call is done; bob's is still live.
    const std::string desc = "alice: Call start: 2026-05-20 14:00:00 Call End: "
                             "2026-05-20 14:10:00\n"
                             "bob: Call start: 2026-05-20 14:12:00 (X.2)\n";
    const auto users = CallLineFormatter::findUsersWithOpenCalls(desc);
    ASSERT_EQ(users.size(), 1U);
    EXPECT_EQ(users[0], UserHandle{"bob"});
}

TEST(CallLineFormatterFindUsersWithOpenCalls, TwoDistinctOpenUsersInFirstSeenOrder) {
    const std::string desc = "alice: Call start: 2026-05-20 14:00:00 (X.1)\n"
                             "bob: Call start: 2026-05-20 14:01:00 (X.2)\n";
    const auto users = CallLineFormatter::findUsersWithOpenCalls(desc);
    ASSERT_EQ(users.size(), 2U);
    EXPECT_EQ(users[0], UserHandle{"alice"});
    EXPECT_EQ(users[1], UserHandle{"bob"});
}

TEST(CallLineFormatterFindUsersWithOpenCalls, SameUserTwiceIsDeduped) {
    const std::string desc = "alice: Call start: 2026-05-20 14:00:00 (X.1)\n"
                             "alice: Call start: 2026-05-20 14:05:00 (X.2)\n";
    const auto users = CallLineFormatter::findUsersWithOpenCalls(desc);
    ASSERT_EQ(users.size(), 1U);
    EXPECT_EQ(users[0], UserHandle{"alice"});
}

TEST(CallLineFormatterFindUsersWithOpenCalls, MalformedStartLineIsSkipped) {
    // Matches the "Call start:" gate but parseStart rejects it (no " (callid)"),
    // so the user must NOT be collected.
    const std::string desc = "ghost: Call start: not-a-timestamp\n"
                             "bob: Call start: 2026-05-20 14:01:00 (X.2)\n";
    const auto users = CallLineFormatter::findUsersWithOpenCalls(desc);
    ASSERT_EQ(users.size(), 1U);
    EXPECT_EQ(users[0], UserHandle{"bob"});
}

} // namespace
