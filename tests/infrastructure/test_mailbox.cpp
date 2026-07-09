#include <gtest/gtest.h>
#include <trantor/net/EventLoop.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "aid/crosscutting/Clock.h"
#include "aid/crosscutting/Logger.h"
#include "aid/infrastructure/Mailbox.h"
#include "aid/infrastructure/Wal.h"
#include "aid/plumbing/Error.h"
#include "aid/plumbing/Result.h"
#include "aid/plumbing/Task.h"
#include "aid/value-types/CallEvent.h"
#include "aid/value-types/Ids.h"

namespace {

using aid::AcceptedCall;
using aid::CallEvent;
using aid::CallId;
using aid::HangupCall;
using aid::IncomingCall;
using aid::OutgoingCall;
using aid::PhoneNumber;
using aid::Timestamp;
using aid::TransferCall;
using aid::UserHandle;
using aid::crosscutting::Clock;
using aid::infrastructure::Mailbox;
using aid::infrastructure::Wal;
using aid::plumbing::Error;
using aid::plumbing::ErrorCode;
using aid::plumbing::Result;
using aid::plumbing::Task;

class FixedClock final : public Clock {
public:
    [[nodiscard]] Timestamp now() const override {
        return Timestamp{std::chrono::milliseconds{1'700'000'000'000}};
    }
};

// Spins an EventLoop on a dedicated thread, mirrors the production
// "domain loop" pattern. EventLoop binds to the thread that
// calls loop(), so we have to construct it inside the worker thread.
class LoopThread {
public:
    LoopThread() {
        std::promise<trantor::EventLoop*> ready;
        auto future = ready.get_future();
        thread_ = std::thread([this, &ready] {
            trantor::EventLoop loop;
            ready.set_value(&loop);
            loop.loop();
        });
        loop_ = future.get();
    }

    LoopThread(const LoopThread&) = delete;
    LoopThread& operator=(const LoopThread&) = delete;
    LoopThread(LoopThread&&) = delete;
    LoopThread& operator=(LoopThread&&) = delete;

    ~LoopThread() {
        if (loop_ != nullptr) {
            loop_->queueInLoop([loop = loop_] { loop->quit(); });
        }
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    [[nodiscard]] trantor::EventLoop& loop() const noexcept { return *loop_; }

private:
    std::thread thread_;
    trantor::EventLoop* loop_{nullptr};
};

struct LoggerOnce {
    LoggerOnce() {
        static std::once_flag flag;
        std::call_once(flag, [] {
            const auto tmp = std::filesystem::temp_directory_path();
            const auto backend = tmp / "aid_mailbox_test_backend.log";
            const auto frontend = tmp / "aid_mailbox_test_frontend.log";
            aid::crosscutting::Logger::initialize(aid::crosscutting::LogLevel::ERROR,
                                                  backend.string(), frontend.string());
        });
    }
};

class MailboxTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto pid = static_cast<std::uint64_t>(::getpid());
        static std::atomic<std::uint64_t> counter{0};
        const auto n = counter.fetch_add(1, std::memory_order_relaxed);
        dir_ = std::filesystem::temp_directory_path() /
               ("aid_mailbox_test_" + std::to_string(pid) + "_" + std::to_string(n));
        std::filesystem::create_directories(dir_);
        walPath_ = (dir_ / "inbox.log").string();
        wal_ = std::make_unique<Wal>(walPath_, clock_);
    }

    void TearDown() override {
        wal_.reset();
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    [[nodiscard]] CallId cid(std::string s) const { return CallId{std::move(s)}; }

    // Drives a no-op handler set; tests override the slot they exercise.
    static Mailbox::Handlers noopHandlers() {
        auto noop = [](auto&&...) -> Task<Result<void>> { co_return Result<void>{}; };
        return Mailbox::Handlers{noop, noop, noop, noop, noop};
    }

    // Spin until predicate is true or wall-clock budget elapses. Used in
    // place of sleeping for "wait for the loop to drain N events" — keeps
    // the test responsive on fast hosts while not deadlocking on slow ones.
    template <class Pred>
    bool waitUntil(Pred&& p, std::chrono::milliseconds budget = std::chrono::milliseconds{2000}) {
        const auto deadline = std::chrono::steady_clock::now() + budget;
        while (std::chrono::steady_clock::now() < deadline) {
            if (p()) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        return false;
    }

    LoggerOnce logger_init_{};
    FixedClock clock_{};
    std::filesystem::path dir_;
    std::string walPath_;
    std::unique_ptr<Wal> wal_;
};

TEST_F(MailboxTest, HappyPath_Single_Incoming) {
    LoopThread lt;
    std::atomic<int> calls{0};

    auto handlers = noopHandlers();
    handlers.incoming = [&](const IncomingCall&, bool) -> Task<Result<void>> {
        calls.fetch_add(1, std::memory_order_relaxed);
        co_return Result<void>{};
    };

    Mailbox mb{lt.loop(), *wal_, aid::crosscutting::Logger::instance(), std::move(handlers),
               nullptr};

    const auto seq = wal_->append(R"({"event":"incoming"})", "cid-h1");
    ASSERT_TRUE(seq.has_value());
    const auto r = mb.enqueue(cid("call-1"),
                              IncomingCall{cid("call-1"), PhoneNumber{"+49"}, PhoneNumber{"+50"}},
                              "cid-h1", *seq);
    ASSERT_TRUE(r.has_value());

    // calls.load() flips inside the handler, but the worker's
    // wal_.ack runs *after* the handler returns. Poll until the worker has
    // also acked seq 1 (which compacts the whole single-callid log) —
    // otherwise the file-empty check is flaky under load (and ASan).
    ASSERT_TRUE(waitUntil([&] {
        if (calls.load() != 1) {
            return false;
        }
        std::ifstream f(walPath_);
        std::string contents((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        return contents.empty();
    }));
    EXPECT_EQ(mb.failedCount(), 0U);
}

TEST_F(MailboxTest, SameCallid_Strict_Order) {
    LoopThread lt;
    std::mutex orderMtx;
    std::vector<std::string> order;
    auto record = [&](std::string tag) {
        std::lock_guard lk{orderMtx};
        order.push_back(std::move(tag));
    };

    Mailbox::Handlers handlers;
    handlers.incoming = [&](const IncomingCall&, bool) -> Task<Result<void>> {
        record("incoming");
        co_return Result<void>{};
    };
    handlers.accepted = [&](const AcceptedCall&) -> Task<Result<void>> {
        record("accepted");
        co_return Result<void>{};
    };
    handlers.transfer = [&](const TransferCall&) -> Task<Result<void>> {
        record("transfer");
        co_return Result<void>{};
    };
    handlers.hangup = [&](const HangupCall&) -> Task<Result<void>> {
        record("hangup");
        co_return Result<void>{};
    };
    handlers.outgoing = [&](const OutgoingCall&, bool) -> Task<Result<void>> {
        record("outgoing");
        co_return Result<void>{};
    };

    Mailbox mb{lt.loop(), *wal_, aid::crosscutting::Logger::instance(), std::move(handlers),
               nullptr};

    const auto s1 = *wal_->append("{}", "cid");
    const auto s2 = *wal_->append("{}", "cid");
    const auto s3 = *wal_->append("{}", "cid");
    const auto s4 = *wal_->append("{}", "cid");

    ASSERT_TRUE(
        mb.enqueue(cid("c"), IncomingCall{cid("c"), PhoneNumber{"a"}, PhoneNumber{"b"}}, "cid", s1)
            .has_value());
    ASSERT_TRUE(mb.enqueue(cid("c"),
                           AcceptedCall{cid("c"), PhoneNumber{"a"}, PhoneNumber{"b"}, std::nullopt},
                           "cid", s2)
                    .has_value());
    ASSERT_TRUE(
        mb.enqueue(cid("c"), TransferCall{cid("c"), UserHandle{"u2"}}, "cid", s3).has_value());
    ASSERT_TRUE(
        mb.enqueue(cid("c"), HangupCall{cid("c"), PhoneNumber{"a"}}, "cid", s4).has_value());

    ASSERT_TRUE(waitUntil([&] {
        std::lock_guard lk{orderMtx};
        return order.size() == 4;
    }));
    std::lock_guard lk{orderMtx};
    ASSERT_EQ(order.size(), 4U);
    EXPECT_EQ(order[0], "incoming");
    EXPECT_EQ(order[1], "accepted");
    EXPECT_EQ(order[2], "transfer");
    EXPECT_EQ(order[3], "hangup");
}

TEST_F(MailboxTest, AllEventsProcessedExactlyOnce_AcrossCallids) {
    LoopThread lt;
    constexpr int kCallids = 5;
    constexpr int kPerCallid = 10;

    std::mutex resMtx;
    std::vector<std::pair<std::string, std::string>> got;

    auto handler = [&](const IncomingCall& e, bool) -> Task<Result<void>> {
        std::lock_guard lk{resMtx};
        got.emplace_back(e.callid.v, e.remote.v);
        co_return Result<void>{};
    };

    auto handlers = noopHandlers();
    handlers.incoming = handler;
    Mailbox mb{lt.loop(), *wal_, aid::crosscutting::Logger::instance(), std::move(handlers),
               nullptr};

    for (int c = 0; c < kCallids; ++c) {
        for (int i = 0; i < kPerCallid; ++i) {
            const auto seq = *wal_->append("{}", "cid");
            ASSERT_TRUE(mb.enqueue(cid("c" + std::to_string(c)),
                                   IncomingCall{cid("c" + std::to_string(c)),
                                                PhoneNumber{std::to_string(i)}, PhoneNumber{"b"}},
                                   "cid", seq)
                            .has_value());
        }
    }

    ASSERT_TRUE(waitUntil([&] {
        std::lock_guard lk{resMtx};
        return got.size() == static_cast<std::size_t>(kCallids * kPerCallid);
    }));

    std::lock_guard lk{resMtx};
    // Each callid's events must appear in submission order.
    for (int c = 0; c < kCallids; ++c) {
        int expected = 0;
        for (const auto& kv : got) {
            if (kv.first == ("c" + std::to_string(c))) {
                EXPECT_EQ(kv.second, std::to_string(expected));
                ++expected;
            }
        }
        EXPECT_EQ(expected, kPerCallid);
    }
}

TEST_F(MailboxTest, Queue_Full_Returns_Error) {
    LoopThread lt;
    auto promise = std::make_shared<std::promise<void>>();
    auto shared = promise->get_future().share();

    std::atomic<int> entered{0};
    std::atomic<int> dispatched{0};

    auto handlers = noopHandlers();
    handlers.incoming = [shared, &dispatched, &entered](const IncomingCall&,
                                                        bool) -> Task<Result<void>> {
        entered.fetch_add(1, std::memory_order_release);
        // TEST-ONLY: synchronous wait blocks the domain loop. Production
        // handlers must co_await upstream RPCs. We use
        // this construct here purely to hold the worker in flight while
        // the test fills the queue.
        shared.wait();
        dispatched.fetch_add(1, std::memory_order_relaxed);
        co_return Result<void>{};
    };

    Mailbox mb{lt.loop(), *wal_, aid::crosscutting::Logger::instance(), std::move(handlers),
               nullptr};

    // Ignition event — wait until the worker has popped it and entered the
    // blocking handler. Without this sync point the test races: the worker
    // might or might not have drained event 0 by the time we start filling.
    const auto seq0 = *wal_->append("{}", "cid");
    ASSERT_TRUE(mb.enqueue(cid("hot"), IncomingCall{cid("hot"), PhoneNumber{"a"}, PhoneNumber{"b"}},
                           "cid", seq0)
                    .has_value());
    ASSERT_TRUE(waitUntil([&] { return entered.load() == 1; }));

    // Fill exactly MAX_QUEUE more events. Worker is blocked, queue is empty
    // to start, so every push lands and the deque grows 0 → MAX_QUEUE.
    for (std::size_t i = 0; i < Mailbox::MAX_QUEUE; ++i) {
        const auto seq = *wal_->append("{}", "cid");
        ASSERT_TRUE(mb.enqueue(cid("hot"),
                               IncomingCall{cid("hot"), PhoneNumber{"a"}, PhoneNumber{"b"}}, "cid",
                               seq)
                        .has_value())
            << "filling event " << i;
    }

    const auto seqOver = *wal_->append("{}", "cid");
    const auto rejected = mb.enqueue(
        cid("hot"), IncomingCall{cid("hot"), PhoneNumber{"a"}, PhoneNumber{"b"}}, "cid", seqOver);
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(rejected.error().code, ErrorCode::InvalidInput);
    EXPECT_EQ(rejected.error().message, "mailbox full");

    promise->set_value();

    // Drain the warm-up event + MAX_QUEUE backlog.
    ASSERT_TRUE(
        waitUntil([&] { return dispatched.load() == 1 + static_cast<int>(Mailbox::MAX_QUEUE); },
                  std::chrono::milliseconds{10000}));
}

TEST_F(MailboxTest, HardCap_Reached) {
    // LoopThread so the EventLoop runs and gets properly torn down. The
    // first event's handler blocks on a promise, pinning the loop so that
    // subsequent enqueues accumulate in queues_ (workers don't drain).
    // walSeq is fabricated — Mailbox's cap check is internal and doesn't
    // touch the WAL; this keeps the test in ms.
    LoopThread lt;
    auto promise = std::make_shared<std::promise<void>>();
    auto shared = promise->get_future().share();

    Mailbox::Handlers handlers;
    // Handler returns an Error after the signal — Mailbox does NOT truncate
    // on a use-case failure, so post-signal drain costs ~50us per event
    // instead of two fsyncs per event (which would push the test past 100 s
    // with 10000 events).
    auto blocker = [shared](auto&&...) -> Task<Result<void>> {
        // TEST-ONLY synchronous wait; see Queue_Full_Returns_Error.
        shared.wait();
        co_return aid::plumbing::unexpected{Error{ErrorCode::Unknown, "test-drain", std::nullopt}};
    };
    handlers.incoming = blocker;
    handlers.outgoing = blocker;
    handlers.accepted = blocker;
    handlers.transfer = blocker;
    handlers.hangup = blocker;

    Mailbox mb{lt.loop(), *wal_, aid::crosscutting::Logger::instance(), std::move(handlers),
               nullptr};

    for (std::size_t i = 0; i < Mailbox::MAX_LIVE_MAILBOXES; ++i) {
        ASSERT_TRUE(mb.enqueue(cid("c" + std::to_string(i)),
                               IncomingCall{cid("c" + std::to_string(i)), PhoneNumber{"a"},
                                            PhoneNumber{"b"}},
                               "cid", static_cast<std::uint64_t>(i + 1))
                        .has_value())
            << "cap fill " << i;
    }

    const auto over = mb.enqueue(cid("overflow"),
                                 IncomingCall{cid("overflow"), PhoneNumber{"a"}, PhoneNumber{"b"}},
                                 "cid", Mailbox::MAX_LIVE_MAILBOXES + 1);
    ASSERT_FALSE(over.has_value());
    EXPECT_EQ(over.error().message, "mailbox cap reached");

    // Existing-callid push at cap must still succeed: the cap is per-NEW
    // callid, not per-enqueue.
    const auto sameCid =
        mb.enqueue(cid("c0"), IncomingCall{cid("c0"), PhoneNumber{"a"}, PhoneNumber{"b"}}, "cid",
                   Mailbox::MAX_LIVE_MAILBOXES + 2);
    ASSERT_TRUE(sameCid.has_value());

    // Release the workers so ~Mailbox can drain. Without this the dtor's
    // barriers time out (2 s each).
    promise->set_value();
}

TEST_F(MailboxTest, Worker_Exception_Logged_Counted_WAL_Preserved) {
    LoopThread lt;
    std::atomic<int> attempts{0};

    auto handlers = noopHandlers();
    handlers.incoming = [&](const IncomingCall&, bool) -> Task<Result<void>> {
        attempts.fetch_add(1, std::memory_order_relaxed);
        throw std::runtime_error("boom");
        co_return Result<void>{};
    };

    Mailbox mb{lt.loop(), *wal_, aid::crosscutting::Logger::instance(), std::move(handlers),
               nullptr};

    const auto seq = *wal_->append(R"({"event":"incoming"})", "cid-x");
    ASSERT_TRUE(mb.enqueue(cid("c"), IncomingCall{cid("c"), PhoneNumber{"a"}, PhoneNumber{"b"}},
                           "cid-x", seq)
                    .has_value());

    ASSERT_TRUE(waitUntil([&] { return mb.failedCount() > 0 && attempts.load() == 1; }));

    EXPECT_EQ(mb.failedCount(), 1U);

    // WAL record must still be present (operator replays after fix).
    std::ifstream f(walPath_);
    std::string contents((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    EXPECT_FALSE(contents.empty());
}

TEST_F(MailboxTest, Usecase_Error_Counted_WAL_Preserved) {
    LoopThread lt;
    std::atomic<int> attempts{0};

    auto handlers = noopHandlers();
    handlers.incoming = [&](const IncomingCall&, bool) -> Task<Result<void>> {
        attempts.fetch_add(1, std::memory_order_relaxed);
        co_return aid::plumbing::unexpected{
            Error{ErrorCode::UpstreamUnavailable, "upstream down", std::nullopt}};
    };

    Mailbox mb{lt.loop(), *wal_, aid::crosscutting::Logger::instance(), std::move(handlers),
               nullptr};

    const auto seq = *wal_->append(R"({"event":"incoming"})", "cid-y");
    ASSERT_TRUE(mb.enqueue(cid("c2"), IncomingCall{cid("c2"), PhoneNumber{"a"}, PhoneNumber{"b"}},
                           "cid-y", seq)
                    .has_value());

    ASSERT_TRUE(waitUntil([&] { return mb.failedCount() > 0 && attempts.load() == 1; }));

    EXPECT_EQ(mb.failedCount(), 1U);

    std::ifstream f(walPath_);
    std::string contents((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    EXPECT_FALSE(contents.empty()) << "Wal must NOT be truncated on use-case failure";
}

TEST_F(MailboxTest, GcDoesNotRemoveCallidsWithPendingEvents) {
    // Workers blocked → queues stay non-empty → GC must NOT remove them
    // even at idle=0. Tightly scoped: this verifies only the "deque
    // non-empty blocks GC" branch. The "GC frees slot when deque empty +
    // no active worker" branch will be covered by an integration test once
    // we have real (async) handlers; with sync handlers, the moment a
    // worker is idle its deque has already been popped to empty and we
    // have no synchronization point to assert from.
    LoopThread lt;
    auto promise = std::make_shared<std::promise<void>>();
    auto shared = promise->get_future().share();
    auto blocker = [shared](auto&&...) -> Task<Result<void>> {
        shared.wait();
        co_return aid::plumbing::unexpected{Error{ErrorCode::Unknown, "test-drain", std::nullopt}};
    };
    Mailbox::Handlers handlers{blocker, blocker, blocker, blocker, blocker};

    Mailbox mb{lt.loop(), *wal_, aid::crosscutting::Logger::instance(), std::move(handlers),
               nullptr};

    for (std::size_t i = 0; i < 5; ++i) {
        ASSERT_TRUE(mb.enqueue(cid("c" + std::to_string(i)),
                               IncomingCall{cid("c" + std::to_string(i)), PhoneNumber{"a"},
                                            PhoneNumber{"b"}},
                               "cid", static_cast<std::uint64_t>(i + 1))
                        .has_value());
    }
    mb.gcIdleOlderThan(std::chrono::seconds{0});
    // All 5 still tracked → liveCount > 0 (1 popped event in worker hand +
    // 4 still in deques). We can't observe the internal map directly, so
    // assert via liveCount, which sums the *deques* — the popped event has
    // already left its deque, so liveCount returns 4.
    EXPECT_GT(mb.liveCount(), 0U);

    promise->set_value();
}

TEST_F(MailboxTest, GcReclaimsIdleMailboxAfterThreshold) {
    // Positive branch (complements GcDoesNotRemoveCallidsWithPendingEvents):
    // a drained, idle callid is reclaimed once it is older than the cutoff,
    // and is NOT reclaimed before then. This is the behavior Main's 1-min
    // gcIdleOlderThan(1h) timer relies on.
    LoopThread lt;
    Mailbox mb{lt.loop(), *wal_, aid::crosscutting::Logger::instance(), noopHandlers(), nullptr};

    const auto seq = wal_->append(R"({"event":"incoming"})", "cid-gc");
    ASSERT_TRUE(seq.has_value());
    ASSERT_TRUE(mb.enqueue(cid("gc-1"),
                           IncomingCall{cid("gc-1"), PhoneNumber{"a"}, PhoneNumber{"b"}}, "cid-gc",
                           *seq)
                    .has_value());

    // drain() polls until activeWorkers_ is empty — a deterministic sync
    // point: the no-op handler is synchronous, so the worker runs to
    // completion before drain returns. The (now-empty) deque and the
    // lastActivity_ entry remain tracked until GC.
    auto drainTask = mb.drain(std::chrono::seconds{2});
    EXPECT_TRUE(drainTask.done());
    ASSERT_EQ(mb.trackedMailboxCount(), 1U) << "drained callid stays tracked until GC";

    // Not idle long enough → the threshold guard keeps it.
    mb.gcIdleOlderThan(std::chrono::hours{1});
    EXPECT_EQ(mb.trackedMailboxCount(), 1U);

    // Past the idle threshold → reclaimed.
    mb.gcIdleOlderThan(std::chrono::seconds{0});
    EXPECT_EQ(mb.trackedMailboxCount(), 0U);
}

TEST_F(MailboxTest, EnqueueRejectsWhileDraining) {
    LoopThread lt;
    Mailbox mb{lt.loop(), *wal_, aid::crosscutting::Logger::instance(), noopHandlers(), nullptr};

    // Kick off drain on a separate thread so it sets draining_ and starts
    // polling. The empty-mailbox case returns immediately.
    auto drainTask = mb.drain(std::chrono::seconds{1});
    // drain() runs synchronously to its first co_await; with empty workers
    // it co_returns immediately, so by the time we reach the next line
    // draining_ is true and the Task is done.
    EXPECT_TRUE(drainTask.done());

    const auto rejected = mb.enqueue(
        cid("late"), IncomingCall{cid("late"), PhoneNumber{"a"}, PhoneNumber{"b"}}, "cid", 1);
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(rejected.error().message, "draining");
}

TEST_F(MailboxTest, Replay_Decodes_And_Dispatches) {
    LoopThread lt;
    std::atomic<int> calls{0};

    auto handlers = noopHandlers();
    handlers.incoming = [&](const IncomingCall&, bool) -> Task<Result<void>> {
        calls.fetch_add(1, std::memory_order_relaxed);
        co_return Result<void>{};
    };

    auto decoder = [](std::string_view body) -> std::optional<CallEvent> {
        // Trivial single-shape decoder for the test fixture; the production
        // decoder is CallController::decode (not yet implemented).
        if (body == R"({"event":"incoming","callid":"replay-1"})") {
            return IncomingCall{CallId{"replay-1"}, PhoneNumber{"+a"}, PhoneNumber{"+b"}};
        }
        return std::nullopt;
    };

    // Append one record so the WAL has something to truncate.
    const auto seq = *wal_->append(R"({"event":"incoming","callid":"replay-1"})", "cid-replay");
    aid::plumbing::WalRecord rec{seq, clock_.now(), "cid-replay",
                                 R"({"event":"incoming","callid":"replay-1"})"};

    Mailbox mb{lt.loop(), *wal_, aid::crosscutting::Logger::instance(), std::move(handlers),
               std::move(decoder)};
    mb.enqueueReplay(rec);

    ASSERT_TRUE(waitUntil([&] {
        if (calls.load() != 1) {
            return false;
        }
        std::ifstream f(walPath_);
        std::string contents((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        return contents.empty();
    })) << "Wal must be truncated after successful replay";
}

} // namespace
