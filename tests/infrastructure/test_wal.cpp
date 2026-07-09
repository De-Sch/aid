#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "aid/crosscutting/Clock.h"
#include "aid/infrastructure/Wal.h"
#include "aid/plumbing/Error.h"
#include "aid/plumbing/WalRecord.h"

namespace {

using aid::crosscutting::Clock;
using aid::infrastructure::Wal;
using aid::plumbing::ErrorCode;
using aid::plumbing::WalRecord;

class FixedClock final : public Clock {
public:
    explicit FixedClock(aid::Timestamp t) : t_(t) {}
    [[nodiscard]] aid::Timestamp now() const override { return t_; }
    void set(aid::Timestamp t) { t_ = t; }

private:
    aid::Timestamp t_;
};

aid::Timestamp tsFromUnix(std::int64_t ms) {
    return aid::Timestamp{std::chrono::milliseconds{ms}};
}

class WalTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Unique temp dir per test under the system temp root.
        const auto pid = static_cast<std::uint64_t>(::getpid());
        static std::atomic<std::uint64_t> counter{0};
        const auto n = counter.fetch_add(1, std::memory_order_relaxed);
        dir_ = std::filesystem::temp_directory_path() /
               ("aid_wal_test_" + std::to_string(pid) + "_" + std::to_string(n));
        std::filesystem::create_directories(dir_);
        path_ = (dir_ / "inbox.log").string();
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    void seed(std::string_view contents) const {
        std::ofstream f(path_, std::ios::binary | std::ios::trunc);
        f.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    }

    [[nodiscard]] std::string slurp() const {
        std::ifstream f(path_, std::ios::binary);
        std::ostringstream oss;
        oss << f.rdbuf();
        return oss.str();
    }

    // Parse the WAL file into the list of seqs it currently holds, in order.
    [[nodiscard]] std::vector<std::uint64_t> seqsOnDisk() const {
        const std::string contents = slurp();
        std::vector<std::uint64_t> seqs;
        std::size_t start = 0;
        for (std::size_t i = 0; i < contents.size(); ++i) {
            if (contents[i] == '\n') {
                const auto r = Wal::parseLine(std::string_view{contents.data() + start, i - start});
                if (r.has_value()) {
                    seqs.push_back(r->seq);
                }
                start = i + 1;
            }
        }
        return seqs;
    }

    [[nodiscard]] static bool hasSeq(const std::vector<std::uint64_t>& seqs, std::uint64_t s) {
        return std::find(seqs.begin(), seqs.end(), s) != seqs.end();
    }

    std::filesystem::path dir_;
    std::string path_;
    FixedClock clock_{tsFromUnix(1'700'000'000'000)};
};

TEST_F(WalTest, RoundTripsSimpleRecord) {
    const WalRecord original{42U, tsFromUnix(1'700'000'000'123),
                             "550e8400-e29b-41d4-a716-446655440000",
                             R"({"event":"Hangup","callid":"x.1"})"};
    const std::string line = Wal::toLine(original);
    ASSERT_FALSE(line.empty());
    ASSERT_EQ(line.back(), '\n');

    const auto parsed = Wal::parseLine(std::string_view{line.data(), line.size() - 1});
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->seq, original.seq);
    EXPECT_EQ(parsed->receivedAt, original.receivedAt);
    EXPECT_EQ(parsed->correlationId, original.correlationId);
    EXPECT_EQ(parsed->body, original.body);
}

TEST_F(WalTest, ParseLineReturnsNulloptOnMalformed) {
    EXPECT_FALSE(Wal::parseLine("not json at all").has_value());
    EXPECT_FALSE(Wal::parseLine(R"({"seq":1})").has_value()); // missing fields
    EXPECT_FALSE(
        Wal::parseLine(R"({"seq":"a","ts":"x","cid":"","body":""})").has_value()); // wrong type
    EXPECT_FALSE(
        Wal::parseLine(R"({"seq":1,"ts":"x","cid":1,"body":""})").has_value()); // cid not string
}

TEST_F(WalTest, ParseLineRejectsEmptyAndWhitespace) {
    EXPECT_FALSE(Wal::parseLine("").has_value());
    EXPECT_FALSE(Wal::parseLine("   \t  ").has_value());
}

TEST_F(WalTest, ColdStartHasEmptyReplayAndSeqOne) {
    Wal wal{path_, clock_};
    EXPECT_EQ(wal.pendingCount(), 0U);
    EXPECT_TRUE(wal.readAll().empty());

    const auto r = wal.append("{}", "cid-1");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, 1U);
}

TEST_F(WalTest, AppendIsMonotonic) {
    Wal wal{path_, clock_};
    const auto a = wal.append("{}", "cid-a");
    const auto b = wal.append("{}", "cid-b");
    const auto c = wal.append("{}", "cid-c");
    ASSERT_TRUE(a && b && c);
    EXPECT_EQ(*a, 1U);
    EXPECT_EQ(*b, 2U);
    EXPECT_EQ(*c, 3U);
}

TEST_F(WalTest, AppendPersistsParseableLine) {
    {
        Wal wal{path_, clock_};
        ASSERT_TRUE(wal.append(R"({"event":"Hangup"})", "cid-x").has_value());
    }
    const std::string contents = slurp();
    ASSERT_FALSE(contents.empty());
    ASSERT_EQ(contents.back(), '\n');
    const auto parsed = Wal::parseLine(std::string_view{contents.data(), contents.size() - 1});
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->seq, 1U);
    EXPECT_EQ(parsed->correlationId, "cid-x");
    EXPECT_EQ(parsed->body, R"({"event":"Hangup"})");
}

TEST_F(WalTest, AppendUsesClockNow) {
    const auto fixed = tsFromUnix(1'700'000'042'000);
    clock_.set(fixed);
    Wal wal{path_, clock_};
    ASSERT_TRUE(wal.append("{}", "cid").has_value());
    const std::string contents = slurp();
    const auto parsed = Wal::parseLine(std::string_view{contents.data(), contents.size() - 1});
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->receivedAt, fixed);
}

TEST_F(WalTest, ReplayPicksUpExistingRecords) {
    {
        Wal w{path_, clock_};
        ASSERT_TRUE(w.append(R"({"n":1})", "cid-1").has_value());
        ASSERT_TRUE(w.append(R"({"n":2})", "cid-2").has_value());
        ASSERT_TRUE(w.append(R"({"n":3})", "cid-3").has_value());
    }
    Wal w2{path_, clock_};
    EXPECT_EQ(w2.pendingCount(), 3U);
    const auto recs = w2.readAll();
    ASSERT_EQ(recs.size(), 3U);
    EXPECT_EQ(recs[0].seq, 1U);
    EXPECT_EQ(recs[0].correlationId, "cid-1");
    EXPECT_EQ(recs[0].body, R"({"n":1})");
    EXPECT_EQ(recs[1].seq, 2U);
    EXPECT_EQ(recs[2].seq, 3U);

    const auto next = w2.append("{}", "cid-4");
    ASSERT_TRUE(next.has_value());
    EXPECT_EQ(*next, 4U);
}

TEST_F(WalTest, ReplaySkipsMalformedLineAndKeepsRest) {
    const WalRecord r1{1, tsFromUnix(1'700'000'001'000), "cid-1", R"({"a":1})"};
    const WalRecord r3{3, tsFromUnix(1'700'000'003'000), "cid-3", R"({"a":3})"};
    const std::string seeded = Wal::toLine(r1) + "this-is-not-json\n" + Wal::toLine(r3);
    seed(seeded);

    Wal w{path_, clock_};
    const auto recs = w.readAll();
    ASSERT_EQ(recs.size(), 2U);
    EXPECT_EQ(recs[0].seq, 1U);
    EXPECT_EQ(recs[1].seq, 3U);

    const auto next = w.append("{}", "cid-next");
    ASSERT_TRUE(next.has_value());
    EXPECT_EQ(*next, 4U);
}

// A crash mid-write leaves a torn trailing record — a truncated JSON
// fragment with no trailing newline after the last fully-flushed line. Replay
// must skip the fragment (parseLine → nullopt) without losing the record that
// was durably written before it, and must not let the fragment's (unparseable)
// seq perturb nextSeq.
TEST_F(WalTest, ReplaySkipsTornTrailingRecordAndKeepsPriorRecord) {
    const WalRecord r1{1, tsFromUnix(1'700'000'001'000), "cid-1", R"({"a":1})"};
    // A record whose write was interrupted partway: valid prefix, abruptly cut,
    // no closing brace, no newline. This is what an fsync'd line followed by a
    // crashed append looks like on disk.
    const std::string torn =
        R"({"seq":2,"ts":"2023-11-14T22:13:21.000Z","cid":"cid-2","body":"{\"eve)";
    seed(Wal::toLine(r1) + torn);

    Wal w{path_, clock_};
    const auto recs = w.readAll();
    ASSERT_EQ(recs.size(), 1U) << "torn trailing fragment must be skipped";
    EXPECT_EQ(recs[0].seq, 1U);
    EXPECT_EQ(recs[0].correlationId, "cid-1");

    // The torn fragment never parsed, so it cannot raise nextSeq: the next
    // append follows the last *valid* record (seq 1), not the torn seq 2.
    const auto next = w.append("{}", "cid-next");
    ASSERT_TRUE(next.has_value());
    EXPECT_EQ(*next, 2U);
}

// Companion case: a single, fully-valid record whose trailing newline never
// made it to disk (clean process exit between write and the newline flush, or a
// final record on a log that was never newline-terminated) must still be
// replayed — std::getline yields the final unterminated line, and it parses.
TEST_F(WalTest, ReplayReadsFinalValidRecordWithoutTrailingNewline) {
    const WalRecord r{5, tsFromUnix(1'700'000'005'000), "cid-5", R"({"event":"Hangup"})"};
    std::string line = Wal::toLine(r); // ends in '\n'
    ASSERT_EQ(line.back(), '\n');
    line.pop_back(); // drop the newline: a complete record with no terminator
    seed(line);

    Wal w{path_, clock_};
    const auto recs = w.readAll();
    ASSERT_EQ(recs.size(), 1U) << "an unterminated but complete final line must not be lost";
    EXPECT_EQ(recs[0].seq, 5U);
    EXPECT_EQ(recs[0].correlationId, "cid-5");
    EXPECT_EQ(recs[0].body, R"({"event":"Hangup"})");

    const auto next = w.append("{}", "cid-next");
    ASSERT_TRUE(next.has_value());
    EXPECT_EQ(*next, 6U);
}

TEST_F(WalTest, ReadAllIsIdempotentlyEmptySecondCall) {
    const WalRecord r{7, tsFromUnix(1'700'000'001'000), "cid", R"({"k":"v"})"};
    seed(Wal::toLine(r));

    Wal w{path_, clock_};
    const auto first = w.readAll();
    ASSERT_EQ(first.size(), 1U);
    const auto second = w.readAll();
    EXPECT_TRUE(second.empty());
}

TEST_F(WalTest, AckRemovesContiguousAckedPrefix) {
    Wal w{path_, clock_};
    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(w.append(R"({})", "cid").has_value());
    }
    // Ack the contiguous prefix 1,2,3 (order does not matter).
    ASSERT_TRUE(w.ack(1).has_value());
    ASSERT_TRUE(w.ack(2).has_value());
    ASSERT_TRUE(w.ack(3).has_value());

    // File now contains seq 4 and 5 only.
    const auto seqs = seqsOnDisk();
    ASSERT_EQ(seqs.size(), 2U);
    EXPECT_EQ(seqs[0], 4U);
    EXPECT_EQ(seqs[1], 5U);

    const auto next = w.append("{}", "cid");
    ASSERT_TRUE(next.has_value());
    EXPECT_EQ(*next, 6U);
}

TEST_F(WalTest, AckLeavesNoTempFile) {
    Wal w{path_, clock_};
    ASSERT_TRUE(w.append("{}", "cid").has_value());
    ASSERT_TRUE(w.append("{}", "cid").has_value());
    ASSERT_TRUE(w.ack(1).has_value());
    EXPECT_FALSE(std::filesystem::exists(path_ + ".tmp"));
    EXPECT_TRUE(std::filesystem::exists(path_));
}

// --- Regression: cross-callid truncate must not drop un-acked seqs ---

TEST_F(WalTest, AckHigherSeqDoesNotDropLowerUnackedRecord) {
    // Interleaved callids share one monotonic seq space:
    //   callid A -> seq 1, 3 ; callid B -> seq 2.
    Wal w{path_, clock_};
    const auto s1 = w.append(R"({"callid":"A.1"})", "cid-A"); // seq 1, callid A
    const auto s2 = w.append(R"({"callid":"B.1"})", "cid-B"); // seq 2, callid B
    const auto s3 = w.append(R"({"callid":"A.1"})", "cid-A"); // seq 3, callid A
    ASSERT_TRUE(s1 && s2 && s3);
    ASSERT_EQ(*s1, 1U);
    ASSERT_EQ(*s2, 2U);
    ASSERT_EQ(*s3, 3U);

    // Worker for callid A finishes seq 3 first and acks it, while callid B's
    // seq 2 and A's own seq 1 are still in flight (un-acked).
    ASSERT_TRUE(w.ack(3).has_value());

    // The un-acked lower seqs MUST survive — this is the at-least-once
    // guarantee the crash window otherwise breaks.
    const auto seqs = seqsOnDisk();
    EXPECT_TRUE(hasSeq(seqs, 1U)) << "seq 1 (callid A, un-acked) was lost";
    EXPECT_TRUE(hasSeq(seqs, 2U)) << "seq 2 (callid B, un-acked) was lost";
    // seq 3 may or may not remain (it is blocked behind the un-acked prefix);
    // either is acceptable — we only require the un-acked records survive.
}

TEST_F(WalTest, AckCompactsContiguousPrefixOnly) {
    Wal w{path_, clock_};
    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(w.append("{}", "cid").has_value());
    }

    // Ack 3 out of order: nothing can be dropped (1 and 2 still un-acked).
    ASSERT_TRUE(w.ack(3).has_value());
    {
        const auto seqs = seqsOnDisk();
        ASSERT_EQ(seqs.size(), 5U);
    }

    // Ack 1: prefix advances to 1, dropping only seq 1.
    ASSERT_TRUE(w.ack(1).has_value());
    {
        const auto seqs = seqsOnDisk();
        ASSERT_EQ(seqs.size(), 4U);
        EXPECT_EQ(seqs.front(), 2U);
    }

    // Ack 2: prefix jumps 1 -> 3 (3 was already acked), dropping 2 and 3.
    ASSERT_TRUE(w.ack(2).has_value());
    {
        const auto seqs = seqsOnDisk();
        ASSERT_EQ(seqs.size(), 2U);
        EXPECT_EQ(seqs[0], 4U);
        EXPECT_EQ(seqs[1], 5U);
    }
}

TEST_F(WalTest, AckIsIdempotent) {
    Wal w{path_, clock_};
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(w.append("{}", "cid").has_value());
    }
    ASSERT_TRUE(w.ack(1).has_value());
    ASSERT_TRUE(w.ack(1).has_value()); // already compacted -> no-op, no error

    const auto seqs = seqsOnDisk();
    ASSERT_EQ(seqs.size(), 2U);
    EXPECT_EQ(seqs[0], 2U);
    EXPECT_EQ(seqs[1], 3U);
    EXPECT_FALSE(std::filesystem::exists(path_ + ".tmp"));
}

TEST_F(WalTest, AckSurvivesRestartReplay) {
    // Seed a file whose smallest seq is 5 (a prior run compacted 1..4), so the
    // replay floor must start ackedPrefix_ at 4.
    const WalRecord r5{5, tsFromUnix(1'700'000'005'000), "cid-A", R"({"callid":"A.1"})"};
    const WalRecord r6{6, tsFromUnix(1'700'000'006'000), "cid-B", R"({"callid":"B.1"})"};
    const WalRecord r7{7, tsFromUnix(1'700'000'007'000), "cid-A", R"({"callid":"A.1"})"};
    seed(Wal::toLine(r5) + Wal::toLine(r6) + Wal::toLine(r7));

    Wal w{path_, clock_};

    // Ack the highest seq first: 5 and 6 are still un-acked and must survive.
    ASSERT_TRUE(w.ack(7).has_value());
    {
        const auto seqs = seqsOnDisk();
        EXPECT_TRUE(hasSeq(seqs, 5U)) << "replayed seq 5 was lost";
        EXPECT_TRUE(hasSeq(seqs, 6U)) << "replayed seq 6 was lost";
    }

    // Acking the rest of the contiguous prefix compacts everything.
    ASSERT_TRUE(w.ack(5).has_value());
    ASSERT_TRUE(w.ack(6).has_value());
    EXPECT_TRUE(seqsOnDisk().empty());
}

TEST_F(WalTest, PendingCountReflectsReplayBacklog) {
    {
        Wal w{path_, clock_};
        for (int i = 0; i < 4; ++i) {
            ASSERT_TRUE(w.append("{}", "cid").has_value());
        }
    }
    Wal w2{path_, clock_};
    EXPECT_EQ(w2.pendingCount(), 4U);
    (void)w2.readAll();
    EXPECT_EQ(w2.pendingCount(), 0U);
}

#ifndef AID_SANITIZE
TEST_F(WalTest, AppendUnderConcurrentCallsProducesUniqueSeqs) {
    Wal w{path_, clock_};
    constexpr int kThreads = 16;
    constexpr int kPerThread = 64;
    std::vector<std::thread> threads;
    std::mutex outMtx;
    std::vector<std::uint64_t> seqs;
    seqs.reserve(static_cast<std::size_t>(kThreads * kPerThread));

    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < kPerThread; ++i) {
                const auto r = w.append("{}", "cid");
                ASSERT_TRUE(r.has_value());
                std::lock_guard lk{outMtx};
                seqs.push_back(*r);
            }
        });
    }
    for (auto& th : threads) {
        th.join();
    }

    ASSERT_EQ(seqs.size(), static_cast<std::size_t>(kThreads * kPerThread));
    std::set<std::uint64_t> unique(seqs.begin(), seqs.end());
    EXPECT_EQ(unique.size(), seqs.size());
    EXPECT_EQ(*unique.begin(), 1U);
    EXPECT_EQ(*unique.rbegin(), static_cast<std::uint64_t>(kThreads * kPerThread));

    // File should be parseable line-by-line with no torn writes.
    const std::string contents = slurp();
    std::size_t start = 0;
    std::size_t lines = 0;
    for (std::size_t i = 0; i < contents.size(); ++i) {
        if (contents[i] == '\n') {
            const auto rec = Wal::parseLine(std::string_view{contents.data() + start, i - start});
            EXPECT_TRUE(rec.has_value());
            start = i + 1;
            ++lines;
        }
    }
    EXPECT_EQ(lines, static_cast<std::size_t>(kThreads * kPerThread));
}
#endif

} // namespace
