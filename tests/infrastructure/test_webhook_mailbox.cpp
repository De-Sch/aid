#include <gtest/gtest.h>
#include <trantor/net/EventLoop.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "aid/crosscutting/Clock.h"
#include "aid/crosscutting/Logger.h"
#include "aid/infrastructure/Wal.h"
#include "aid/infrastructure/WebhookMailbox.h"
#include "aid/plumbing/Error.h"
#include "aid/plumbing/Result.h"
#include "aid/plumbing/Task.h"
#include "aid/value-types/Ids.h"

namespace {

using aid::TicketId;
using aid::crosscutting::Clock;
using aid::crosscutting::Logger;
using aid::infrastructure::Wal;
using aid::infrastructure::WebhookMailbox;
using aid::plumbing::Result;
using aid::plumbing::Task;

class FixedClock final : public Clock {
public:
    [[nodiscard]] aid::Timestamp now() const override {
        return aid::Timestamp{std::chrono::milliseconds{1'700'000'000'000}};
    }
};

class LoopThread {
public:
    LoopThread() {
        std::promise<trantor::EventLoop*> ready;
        auto future = ready.get_future();
        thread_ = std::thread([&ready] {
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
            Logger::initialize(aid::crosscutting::LogLevel::ERROR,
                               (tmp / "aid_webhook_mb_test_backend.log").string(),
                               (tmp / "aid_webhook_mb_test_frontend.log").string());
        });
    }
};

class WebhookMailboxTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto pid = static_cast<std::uint64_t>(::getpid());
        static std::atomic<std::uint64_t> counter{0};
        const auto n = counter.fetch_add(1, std::memory_order_relaxed);
        dir_ = std::filesystem::temp_directory_path() /
               ("aid_webhook_mb_test_" + std::to_string(pid) + "_" + std::to_string(n));
        std::filesystem::create_directories(dir_);
        wal_ = std::make_unique<Wal>((dir_ / "webhook.log").string(), clock_);
    }
    void TearDown() override {
        mb_.reset();
        wal_.reset();
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    LoggerOnce loggerInit_{};
    FixedClock clock_{};
    std::filesystem::path dir_;
    std::unique_ptr<Wal> wal_;
    LoopThread loop_;
    std::unique_ptr<WebhookMailbox> mb_;
};

TEST_F(WebhookMailboxTest, HandlerReceivesPayloadAndAcksWal) {
    std::mutex m;
    std::vector<std::string> seen;
    auto handler = [&](std::string payload, std::string) -> Task<Result<void>> {
        {
            std::lock_guard lk{m};
            seen.push_back(std::move(payload));
        }
        co_return Result<void>{};
    };
    mb_ = std::make_unique<WebhookMailbox>(loop_.loop(), *wal_, Logger::instance(),
                                           std::move(handler), nullptr);

    auto seq = wal_->append(R"({"work_package":{"id":1}})", "cid-1");
    ASSERT_TRUE(seq.has_value());
    ASSERT_TRUE(mb_->enqueue(TicketId{"1"}, R"({"work_package":{"id":1}})", "cid-1", *seq));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (std::chrono::steady_clock::now() < deadline) {
        if (wal_->pendingCount() == 0 && mb_->liveCount() == 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    std::lock_guard lk{m};
    ASSERT_EQ(seen.size(), 1U);
    EXPECT_EQ(seen.front(), R"({"work_package":{"id":1}})");
    EXPECT_EQ(wal_->pendingCount(), 0U) << "successful handler acks the WAL record";
}

TEST_F(WebhookMailboxTest, BackpressureFullQueueRejectsWithError) {
    // Deterministic backpressure test — mirrors test_mailbox.cpp
    // Queue_Full_Returns_Error. The earlier version raced: it fired MAX_QUEUE+2
    // enqueues in a tight loop and asserted the *last* one was rejected, but if
    // the worker's single pull landed between the queue filling and that last
    // enqueue, a freed slot let it land instead — an intermittent failure under
    // parallel ctest load. The fix: an `entered` sync point so the worker has
    // provably popped its one item and is blocked BEFORE we fill, making the
    // overflow enqueue deterministically the rejected one.
    std::promise<void> gate;
    auto gateFut = gate.get_future().share();
    std::atomic<int> entered{0};
    std::atomic<int> handled{0};

    // Returns an Error so the worker never acks the synthetic WAL seqs used
    // below (no real records were appended); we only care about enqueue
    // backpressure here, not the success/ack path.
    auto handler = [gateFut, &entered, &handled](std::string, std::string) -> Task<Result<void>> {
        entered.fetch_add(1, std::memory_order_release);
        // TEST-ONLY synchronous wait — holds the worker in flight while the
        // test fills the queue. Production handlers co_await.
        gateFut.wait();
        handled.fetch_add(1, std::memory_order_release);
        co_return aid::plumbing::unexpected{aid::plumbing::Error{
            aid::plumbing::ErrorCode::InvalidInput, "test: drop", std::nullopt}};
    };
    mb_ = std::make_unique<WebhookMailbox>(loop_.loop(), *wal_, Logger::instance(),
                                           std::move(handler), nullptr);

    const auto waitUntil = [](auto pred) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (!pred() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        return pred();
    };

    // Ignition event — wait until the worker has popped it and is blocked in the
    // handler. Without this sync point a slot freed mid-fill would let the
    // "over" enqueue land instead of rejecting.
    ASSERT_TRUE(mb_->enqueue(TicketId{"42"}, "{}", "cid", 1).has_value());
    ASSERT_TRUE(waitUntil([&] { return entered.load(std::memory_order_acquire) == 1; }))
        << "worker must pop the ignition event and block before we fill the queue";

    // Worker is blocked and the deque is empty → every push lands, growing the
    // deque 0 → MAX_QUEUE.
    for (std::size_t i = 0; i < WebhookMailbox::MAX_QUEUE; ++i) {
        ASSERT_TRUE(mb_->enqueue(TicketId{"42"}, "{}", "cid", static_cast<std::uint64_t>(i + 2))
                        .has_value())
            << "filling event " << i;
    }

    // The (MAX_QUEUE+1)-th push now deterministically overflows.
    const auto rejected = mb_->enqueue(TicketId{"42"}, "{}", "cid",
                                       static_cast<std::uint64_t>(WebhookMailbox::MAX_QUEUE + 2));
    ASSERT_FALSE(rejected.has_value()) << "queue at MAX_QUEUE must reject for backpressure";
    EXPECT_EQ(rejected.error().code, aid::plumbing::ErrorCode::InvalidInput);
    EXPECT_EQ(rejected.error().message, "webhook mailbox full");

    // Release the worker and drain the ignition event + MAX_QUEUE backlog, so no
    // handler is mid-flight (holding &entered/&handled) when the test returns.
    gate.set_value();
    ASSERT_TRUE(waitUntil([&] {
        return handled.load(std::memory_order_acquire) ==
               1 + static_cast<int>(WebhookMailbox::MAX_QUEUE);
    })) << "worker must drain the whole backlog after the gate opens";
}

TEST_F(WebhookMailboxTest, DrainingRejectsNewEnqueue) {
    auto handler = [](std::string, std::string) -> Task<Result<void>> { co_return Result<void>{}; };
    mb_ = std::make_unique<WebhookMailbox>(loop_.loop(), *wal_, Logger::instance(),
                                           std::move(handler), nullptr);
    auto t = mb_->drain(std::chrono::seconds{1});
    ASSERT_TRUE(t.done());
    EXPECT_FALSE(mb_->enqueue(TicketId{"1"}, "{}", "cid", 1)) << "draining mailbox rejects enqueue";
}

} // namespace
