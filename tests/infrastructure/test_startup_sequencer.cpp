#include <gtest/gtest.h>

#include "aid/infrastructure/StartupSequencer.h"
#include "aid/plumbing/Error.h"

namespace {

using aid::infrastructure::StartupSequencer;
using aid::plumbing::ErrorCode;

// The guard main() uses to assert WAL replay precedes listener open.

TEST(StartupSequencer, FreshGuardReportsReplayNotComplete) {
    StartupSequencer seq;
    EXPECT_FALSE(seq.replayComplete());
}

TEST(StartupSequencer, MarkThenRequire_Ok) {
    StartupSequencer seq;
    seq.markReplayComplete();
    EXPECT_TRUE(seq.replayComplete());

    const auto ordered = seq.requireReplayedBeforeListening();
    EXPECT_TRUE(ordered.has_value()) << "replay marked complete → listeners may open";
}

TEST(StartupSequencer, RequireWithoutMark_InvariantViolation) {
    StartupSequencer seq;

    const auto ordered = seq.requireReplayedBeforeListening();
    ASSERT_FALSE(ordered.has_value()) << "listeners must not open before replay";
    EXPECT_EQ(ordered.error().code, ErrorCode::InvariantViolation);
}

} // namespace
