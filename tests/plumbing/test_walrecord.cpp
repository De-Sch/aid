#include <gtest/gtest.h>

#include <chrono>
#include <string>

#include "aid/plumbing/WalRecord.h"

using aid::plumbing::WalRecord;

TEST(WalRecord, DefaultsAreZeroSeqEmptyStrings) {
    WalRecord r;
    EXPECT_EQ(0u, r.seq);
    EXPECT_TRUE(r.correlationId.empty());
    EXPECT_TRUE(r.body.empty());
}

TEST(WalRecord, FieldsRoundTripViaCopy) {
    const auto now = std::chrono::system_clock::now();
    WalRecord src;
    src.seq = 17;
    src.receivedAt = now;
    src.correlationId = "cid-xyz";
    src.body = R"({"event":"INCOMING","callid":"abc"})";

    WalRecord copy = src;
    EXPECT_EQ(17u, copy.seq);
    EXPECT_EQ(now, copy.receivedAt);
    EXPECT_EQ("cid-xyz", copy.correlationId);
    EXPECT_EQ(src.body, copy.body);
}
