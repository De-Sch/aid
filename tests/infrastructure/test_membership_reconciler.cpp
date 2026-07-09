#include <gtest/gtest.h>
#include <trantor/net/EventLoop.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "FakeTicketStore.h"
#include "aid/crosscutting/Logger.h"
#include "aid/infrastructure/MembershipReconciler.h"
#include "aid/plumbing/Error.h"
#include "aid/plumbing/Result.h"
#include "aid/plumbing/Task.h"
#include "aid/value-types/Ids.h"
#include "aid/value-types/MembershipDelta.h"

namespace {

using aid::MembershipDelta;
using aid::ProjectId;
using aid::UserHandle;
using aid::crosscutting::Logger;
using aid::fakes::FakeTicketStore;
using aid::infrastructure::MembershipReconciler;
using aid::plumbing::Error;
using aid::plumbing::ErrorCode;
using aid::plumbing::Result;
using aid::plumbing::Task;

// Side-thread EventLoop, mirroring the daemon's DomainLoop / the health-service
// test's LoopThread. The reconciler schedules its timer + tick coroutines here.
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
            Logger::initialize(aid::crosscutting::LogLevel::ERROR,
                               "/tmp/aid_membership_reconciler_test_backend.log",
                               "/tmp/aid_membership_reconciler_test_frontend.log");
        });
    }
};

class MembershipReconcilerTest : public ::testing::Test {
protected:
    LoggerOnce loggerOnce_{};
    LoopThread loop_{};
    FakeTicketStore ts_{};

    // Injected gate + diff→push, observed from the test thread.
    std::atomic<bool> gateReturns_{true};
    std::atomic<int> gateCalls_{0};
    std::atomic<int> applyCalls_{0};
    std::mutex appliedMtx_;
    std::vector<MembershipDelta> appliedDeltas_;

    std::unique_ptr<MembershipReconciler> reconciler_;

    void makeReconciler(std::chrono::seconds interval = std::chrono::seconds{3600}) {
        auto gate = [this]() {
            gateCalls_.fetch_add(1, std::memory_order_acq_rel);
            return gateReturns_.load(std::memory_order_acquire);
        };
        auto apply = [this](std::vector<MembershipDelta> deltas) -> Task<Result<void>> {
            {
                std::lock_guard<std::mutex> lk(appliedMtx_);
                appliedDeltas_ = std::move(deltas);
            }
            // release-paired with the test thread's acquire load on applyCalls_.
            applyCalls_.fetch_add(1, std::memory_order_release);
            co_return Result<void>{};
        };
        reconciler_ = std::make_unique<MembershipReconciler>(
            loop_.loop(), ts_, std::move(gate), std::move(apply), Logger::instance(), interval);
    }

    void TearDown() override {
        if (reconciler_) {
            reconciler_->stop(); // cancel timer + drain in-flight while loop alive.
            reconciler_.reset();
        }
    }

    // Post a no-op onto the loop and wait for it to run. Because queueInLoop is
    // FIFO and the fake store never really suspends, a kick()'s tick coroutine
    // runs to completion BEFORE this barrier executes — so after barrier()
    // returns, all counters written on the loop thread are visible here (the
    // barrier's release/acquire forms the happens-before edge).
    [[nodiscard]] bool barrier(std::chrono::milliseconds timeout = std::chrono::seconds{2}) {
        std::atomic<bool> done{false};
        loop_.loop().queueInLoop([&done] { done.store(true, std::memory_order_release); });
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!done.load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() > deadline) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
        }
        return true;
    }
};

[[nodiscard]] MembershipDelta makeDelta(std::string project, std::vector<UserHandle> added,
                                        std::vector<UserHandle> removed) {
    MembershipDelta d;
    d.project = ProjectId{std::move(project)};
    d.added = std::move(added);
    d.removed = std::move(removed);
    return d;
}

} // namespace

// Gate closed (no dashboard connected) ⇒ the heavy refreshMembership poll must
// NOT run. This is the "zero round-trips while idle" invariant.
TEST_F(MembershipReconcilerTest, NoConnectionsSkipsRefreshMembership) {
    makeReconciler();
    gateReturns_.store(false, std::memory_order_release);

    reconciler_->kick();
    ASSERT_TRUE(barrier());

    EXPECT_GE(gateCalls_.load(std::memory_order_acquire), 1); // the tick consulted the gate
    EXPECT_EQ(ts_.refreshMembership_calls, 0);                // …and stopped there
    EXPECT_EQ(applyCalls_.load(std::memory_order_acquire), 0);
}

// Connect-kick with a real change ⇒ one refresh, deltas fed to the diff→push.
TEST_F(MembershipReconcilerTest, ConnectKickReconcilesWhenMembershipChanged) {
    makeReconciler();
    gateReturns_.store(true, std::memory_order_release);
    ts_.nextRefreshMembership.push_back(Result<std::vector<MembershipDelta>>{
        std::vector<MembershipDelta>{makeDelta("7", {UserHandle{"alice"}}, {})}});

    reconciler_->kick();
    ASSERT_TRUE(barrier());

    EXPECT_EQ(ts_.refreshMembership_calls, 1);
    ASSERT_EQ(applyCalls_.load(std::memory_order_acquire), 1);
    std::lock_guard<std::mutex> lk(appliedMtx_);
    ASSERT_EQ(appliedDeltas_.size(), 1u);
    EXPECT_EQ(appliedDeltas_.at(0).project, ProjectId{"7"});
    ASSERT_EQ(appliedDeltas_.at(0).added.size(), 1u);
    EXPECT_EQ(appliedDeltas_.at(0).added.at(0), UserHandle{"alice"});
}

// Refresh succeeds but nothing changed ⇒ the diff→push step is skipped entirely
// (no wasted openCallsInProject fan-out).
TEST_F(MembershipReconcilerTest, EmptyDeltasSkipsApply) {
    makeReconciler();
    gateReturns_.store(true, std::memory_order_release);
    ts_.nextRefreshMembership.push_back(
        Result<std::vector<MembershipDelta>>{std::vector<MembershipDelta>{}});

    reconciler_->kick();
    ASSERT_TRUE(barrier());

    EXPECT_EQ(ts_.refreshMembership_calls, 1);
    EXPECT_EQ(applyCalls_.load(std::memory_order_acquire), 0);
}

// A failed refresh is surfaced (logged) but never crashes and never invents
// deltas — the safety guard lives in the plugin; here we just don't apply.
TEST_F(MembershipReconcilerTest, RefreshErrorIsBestEffortNoApply) {
    makeReconciler();
    gateReturns_.store(true, std::memory_order_release);
    ts_.nextRefreshMembership.push_back(Result<std::vector<MembershipDelta>>{
        aid::plumbing::unexpected(Error{ErrorCode::UpstreamUnavailable, "boom", std::nullopt})});

    reconciler_->kick();
    ASSERT_TRUE(barrier());

    EXPECT_EQ(ts_.refreshMembership_calls, 1);
    EXPECT_EQ(applyCalls_.load(std::memory_order_acquire), 0);
}

// After stop(), a kick() must not start a new tick (timer cancelled + gate path
// short-circuited). Guards the teardown contract that nothing resumes on a
// soon-to-be-destroyed reconciler.
TEST_F(MembershipReconcilerTest, KickAfterStopIsInert) {
    makeReconciler();
    gateReturns_.store(true, std::memory_order_release);
    reconciler_->stop();

    reconciler_->kick();
    ASSERT_TRUE(barrier());

    EXPECT_EQ(gateCalls_.load(std::memory_order_acquire), 0);
    EXPECT_EQ(ts_.refreshMembership_calls, 0);
}

// start() schedules the recurring poll without firing immediately (a long
// interval), and stop() cancels it cleanly. Exercises the timer lifecycle.
TEST_F(MembershipReconcilerTest, StartSchedulesTimerAndStopCancelsCleanly) {
    makeReconciler(std::chrono::seconds{3600});
    gateReturns_.store(true, std::memory_order_release);

    reconciler_->start();
    ASSERT_TRUE(barrier());

    // The 1-hour timer hasn't fired, so no poll yet.
    EXPECT_EQ(ts_.refreshMembership_calls, 0);
    EXPECT_NO_THROW(reconciler_->stop()); // idempotent, clean cancel.
}
