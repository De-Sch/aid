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

#include "FakeAddressBook.h"
#include "FakeTicketStore.h"
#include "aid/crosscutting/Clock.h"
#include "aid/crosscutting/Logger.h"
#include "aid/infrastructure/HealthService.h"
#include "aid/infrastructure/Mailbox.h"
#include "aid/infrastructure/Wal.h"
#include "aid/plumbing/Error.h"
#include "aid/plumbing/Result.h"
#include "aid/plumbing/Task.h"
#include "aid/value-types/Ids.h"

namespace {

using aid::Timestamp;
using aid::crosscutting::Clock;
using aid::fakes::FakeAddressBook;
using aid::fakes::FakeTicketStore;
using aid::infrastructure::HealthService;
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
            aid::crosscutting::Logger::initialize(aid::crosscutting::LogLevel::ERROR,
                                                  (tmp / "aid_health_test_backend.log").string(),
                                                  (tmp / "aid_health_test_frontend.log").string());
        });
    }
};

class HealthServiceTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto pid = static_cast<std::uint64_t>(::getpid());
        static std::atomic<std::uint64_t> counter{0};
        const auto n = counter.fetch_add(1, std::memory_order_relaxed);
        dir_ = std::filesystem::temp_directory_path() /
               ("aid_health_test_" + std::to_string(pid) + "_" + std::to_string(n));
        std::filesystem::create_directories(dir_);
        walPath_ = (dir_ / "inbox.log").string();
        wal_ = std::make_unique<Wal>(walPath_, clock_);

        auto noop = [](auto&&...) -> Task<Result<void>> { co_return Result<void>{}; };
        Mailbox::Handlers handlers{noop, noop, noop, noop, noop};
        mailbox_ =
            std::make_unique<Mailbox>(loop_.loop(), *wal_, aid::crosscutting::Logger::instance(),
                                      std::move(handlers), nullptr);
    }

    void TearDown() override {
        mailbox_.reset();
        wal_.reset();
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    [[nodiscard]] std::unique_ptr<HealthService> makeService(bool pluginsLoaded = true) {
        return std::make_unique<HealthService>(ts_, ab_, *mailbox_, pluginsLoaded);
    }

    LoggerOnce logger_init_{};
    FixedClock clock_{};
    LoopThread loop_{};
    std::filesystem::path dir_;
    std::string walPath_;
    std::unique_ptr<Wal> wal_;
    std::unique_ptr<Mailbox> mailbox_;
    FakeTicketStore ts_{};
    FakeAddressBook ab_{};
};

TEST_F(HealthServiceTest, BeforeBootstrapPing_StatusStarting) {
    auto svc = makeService();
    const auto snap = svc->current();
    EXPECT_EQ(snap.status, "starting");
    EXPECT_EQ(snap.ticket_system, "unreachable");
    EXPECT_EQ(snap.address_system, "unreachable");
    EXPECT_TRUE(snap.plugins_loaded);
    EXPECT_EQ(snap.queued_events, 0U);
    EXPECT_EQ(snap.failed_events, 0U);
    EXPECT_EQ(ts_.ping_calls, 0);
    EXPECT_EQ(ab_.ping_calls, 0);
}

TEST_F(HealthServiceTest, PluginsLoadedFlag_FalsePropagates) {
    auto svc = makeService(false);
    EXPECT_FALSE(svc->current().plugins_loaded);
}

TEST_F(HealthServiceTest, BothUpstreamsOk_StatusOk) {
    auto svc = makeService();
    ts_.nextPing.push_back(Result<void>{});
    ab_.nextPing.push_back(Result<void>{});

    // bootstrapPing() runs eagerly to completion here because the fake
    // ports' ping() coroutines never suspend (initial_suspend = suspend_never
    // + synchronous co_return). Same pattern as tests/ports/test_ticketstore.cpp.
    // In production main() must co_await on the domain loop.
    auto t = svc->bootstrapPing();
    ASSERT_TRUE(t.done());

    const auto snap = svc->current();
    EXPECT_EQ(snap.status, "ok");
    EXPECT_EQ(snap.ticket_system, "reachable");
    EXPECT_EQ(snap.address_system, "reachable");
    EXPECT_EQ(ts_.ping_calls, 1);
    EXPECT_EQ(ab_.ping_calls, 1);
}

TEST_F(HealthServiceTest, TicketSystemFails_StatusDegraded) {
    auto svc = makeService();
    ts_.nextPing.push_back(
        aid::plumbing::unexpected{Error{ErrorCode::UpstreamUnavailable, "boom", std::nullopt}});
    ab_.nextPing.push_back(Result<void>{});

    auto t = svc->bootstrapPing();
    ASSERT_TRUE(t.done());

    const auto snap = svc->current();
    EXPECT_EQ(snap.status, "degraded");
    EXPECT_EQ(snap.ticket_system, "unreachable");
    EXPECT_EQ(snap.address_system, "reachable");
}

TEST_F(HealthServiceTest, AddressSystemFails_StatusDegraded) {
    auto svc = makeService();
    ts_.nextPing.push_back(Result<void>{});
    ab_.nextPing.push_back(
        aid::plumbing::unexpected{Error{ErrorCode::UpstreamUnavailable, "boom", std::nullopt}});

    auto t = svc->bootstrapPing();
    ASSERT_TRUE(t.done());

    const auto snap = svc->current();
    EXPECT_EQ(snap.status, "degraded");
    EXPECT_EQ(snap.ticket_system, "reachable");
    EXPECT_EQ(snap.address_system, "unreachable");
}

TEST_F(HealthServiceTest, BothFail_StatusDegraded) {
    auto svc = makeService();
    ts_.nextPing.push_back(
        aid::plumbing::unexpected{Error{ErrorCode::UpstreamUnavailable, "boom", std::nullopt}});
    ab_.nextPing.push_back(
        aid::plumbing::unexpected{Error{ErrorCode::UpstreamUnavailable, "boom", std::nullopt}});

    auto t = svc->bootstrapPing();
    ASSERT_TRUE(t.done());

    const auto snap = svc->current();
    EXPECT_EQ(snap.status, "degraded");
    EXPECT_EQ(snap.ticket_system, "unreachable");
    EXPECT_EQ(snap.address_system, "unreachable");
}

TEST_F(HealthServiceTest, LiveCountersDelegateToMailbox) {
    auto svc = makeService();
    // Empty Mailbox at construction: queued + failed both zero, regardless
    // of whether bootstrapPing() has run.
    EXPECT_EQ(svc->current().queued_events, mailbox_->liveCount());
    EXPECT_EQ(svc->current().failed_events, mailbox_->failedCount());
    EXPECT_EQ(svc->current().queued_events, 0U);
    EXPECT_EQ(svc->current().failed_events, 0U);
}

TEST_F(HealthServiceTest, UptimeNonNegativeAndMonotonic) {
    auto svc = makeService();
    const auto a = svc->current().uptime_s;
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    const auto b = svc->current().uptime_s;
    EXPECT_GE(a, 0);
    EXPECT_GE(b, a);
}

} // namespace
