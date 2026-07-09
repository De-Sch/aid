#include <gtest/gtest.h>

#include <coroutine>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#include "aid/plumbing/Task.h"

using aid::plumbing::Task;

namespace {

// Minimal stand-in for trantor::EventLoop exposing only the two members
// ResumeOnAwaiter touches. Lets us exercise resumeOn() without linking Drogon
// (the plumbing test target deliberately depends on aid_plumbing alone).
struct FakeLoop {
    bool inThread = false;
    std::vector<std::function<void()>> queued;

    [[nodiscard]] bool isInLoopThread() const noexcept { return inThread; }
    void queueInLoop(std::function<void()> f) { queued.push_back(std::move(f)); }
    void drain() {
        auto pending = std::move(queued);
        queued.clear();
        for (auto& f : pending) {
            f();
        }
    }
};

Task<void> hopOnto(FakeLoop* loop, bool& resumed) {
    co_await aid::plumbing::resumeOn(loop);
    resumed = true;
}

// Manually-suspending awaiter — lets a test exercise the path where the
// inner Task is *not* yet done when the outer co_awaits it, forcing the
// symmetric-transfer branch in FinalAwaiter::await_suspend.
struct ManualAwaiter {
    std::coroutine_handle<>* sink;
    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> h) noexcept { *sink = h; }
    void await_resume() const noexcept {}
};

Task<int> immediateInt(int v) {
    co_return v;
}

Task<std::string> immediateString(std::string s) {
    co_return s;
}

Task<void> immediateVoid() {
    co_return;
}

Task<int> throwingInt() {
    throw std::runtime_error("boom");
    co_return 0;
}

Task<int> awaitInner() {
    int v = co_await immediateInt(42);
    co_return v + 1;
}

Task<void> drain(Task<int> inner, int& sink) {
    sink = co_await inner;
}

Task<void> drainVoid(Task<void> inner, bool& flag) {
    co_await inner;
    flag = true;
}

Task<void> drainThrowing(Task<int> inner, bool& threw) {
    try {
        (void)co_await inner;
    } catch (const std::runtime_error&) {
        threw = true;
    }
}

Task<int> suspendingInt(std::coroutine_handle<>* sink) {
    co_await ManualAwaiter{sink};
    co_return 7;
}

} // namespace

TEST(Task, EagerExecutionRunsToCompletionImmediately) {
    auto t = immediateInt(42);
    EXPECT_TRUE(t.done());
}

TEST(Task, CoAwaitYieldsValue) {
    int sink = 0;
    auto outer = drain(immediateInt(42), sink);
    EXPECT_TRUE(outer.done());
    EXPECT_EQ(42, sink);
}

TEST(Task, NestedAwaitYieldsValue) {
    int sink = 0;
    auto outer = drain(awaitInner(), sink);
    EXPECT_TRUE(outer.done());
    EXPECT_EQ(43, sink);
}

TEST(Task, VoidSpecializationCompletes) {
    bool flag = false;
    auto outer = drainVoid(immediateVoid(), flag);
    EXPECT_TRUE(outer.done());
    EXPECT_TRUE(flag);
}

TEST(Task, PropagatesExceptionThroughAwaitResume) {
    bool threw = false;
    auto outer = drainThrowing(throwingInt(), threw);
    EXPECT_TRUE(outer.done());
    EXPECT_TRUE(threw);
}

TEST(Task, MoveOnlyDestructorReleasesFrame) {
    Task<std::string> a = immediateString("hello");
    Task<std::string> b = std::move(a);
    EXPECT_TRUE(b.done());
    EXPECT_TRUE(a.done()); // moved-from: handle null, done() reports true
}

TEST(Task, FireAndForgetDoesNotLeak) {
    // Discarding a completed Task hits the FinalAwaiter self-destruct
    // path indirectly (the dtor destroys the suspended frame). Run this
    // test under ASan; if the frame leaked, ASan reports it.
    for (int i = 0; i < 10; ++i) {
        (void)immediateInt(i);
    }
    SUCCEED();
}

TEST(ResumeOn, DefersContinuationOntoLoopWhenOffThread) {
    // Not in the loop's thread: the awaiter must suspend and re-post the
    // continuation via queueInLoop — the code after the co_await must NOT have
    // run yet when hopOnto returns control.
    FakeLoop loop;
    loop.inThread = false;
    bool resumed = false;

    auto t = hopOnto(&loop, resumed);
    EXPECT_FALSE(resumed);
    ASSERT_EQ(loop.queued.size(), 1U);
    EXPECT_FALSE(t.done());

    loop.drain(); // the loop runs the posted continuation
    EXPECT_TRUE(resumed);
    EXPECT_TRUE(t.done());
}

TEST(ResumeOn, ResumesInlineWhenAlreadyOnLoopThread) {
    // Already on the loop's thread: no re-post, the continuation runs inline.
    FakeLoop loop;
    loop.inThread = true;
    bool resumed = false;

    auto t = hopOnto(&loop, resumed);
    EXPECT_TRUE(resumed);
    EXPECT_TRUE(loop.queued.empty());
    EXPECT_TRUE(t.done());
}

TEST(ResumeOn, ResumesInlineWhenLoopIsNull) {
    // No loop to hop to (e.g. a unit test with no running event loop): inline.
    bool resumed = false;
    auto t = hopOnto(nullptr, resumed);
    EXPECT_TRUE(resumed);
    EXPECT_TRUE(t.done());
}

TEST(Task, SuspendsAndResumesViaContinuation) {
    std::coroutine_handle<> manual;
    int sink = 0;
    auto outer = drain(suspendingInt(&manual), sink);
    // Inner Task hit ManualAwaiter and suspended; outer is parked waiting
    // on inner's final_suspend continuation. Resume the inner — it co_returns
    // 7, FinalAwaiter symmetric-transfers to outer's continuation, outer
    // reads the value and completes.
    EXPECT_FALSE(outer.done());
    ASSERT_TRUE(manual);
    manual.resume();
    EXPECT_TRUE(outer.done());
    EXPECT_EQ(7, sink);
}
