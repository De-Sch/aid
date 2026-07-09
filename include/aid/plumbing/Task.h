#pragma once

#include <coroutine>
#include <exception>
#include <optional>
#include <utility>

// Minimal C++20 coroutine return type. Uses only <coroutine> so it can
// cross the plugin ABI without plugins linking libdrogon.

namespace aid::plumbing {

namespace detail {

struct FinalAwaiter {
    bool await_ready() const noexcept { return false; }

    template <class P> std::coroutine_handle<> await_suspend(std::coroutine_handle<P> h) noexcept {
        // Symmetric-transfer to the waiting continuation if any;
        // otherwise suspend in place. The Task RAII wrapper owns frame
        // destruction in both paths — see Task::~Task. Destroying here
        // would double-free with the dtor.
        auto& promise = h.promise();
        if (promise.continuation) {
            return promise.continuation;
        }
        return std::noop_coroutine();
    }

    void await_resume() const noexcept {}
};

} // namespace detail

template <class T> class [[nodiscard]] Task {
public:
    struct promise_type {
        std::optional<T> value;
        std::exception_ptr exception;
        std::coroutine_handle<> continuation;

        Task get_return_object() {
            return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        std::suspend_never initial_suspend() noexcept { return {}; }
        detail::FinalAwaiter final_suspend() noexcept { return {}; }

        template <class U> void return_value(U&& v) { value.emplace(std::forward<U>(v)); }

        void unhandled_exception() noexcept { exception = std::current_exception(); }
    };

    Task() noexcept = default;
    explicit Task(std::coroutine_handle<promise_type> handle) noexcept : h_(handle) {}

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;
    // Move-assignment deliberately deleted: tearing down a Task whose
    // promise.continuation has already been set by an awaiter would
    // leave the awaiter pointing at a destroyed frame. Move-construction
    // (transfer of fresh ownership) is the only mutation we need.
    Task(Task&& other) noexcept : h_(std::exchange(other.h_, {})) {}
    Task& operator=(Task&&) = delete;

    ~Task() {
        if (h_) {
            h_.destroy();
        }
    }

    [[nodiscard]] bool done() const noexcept { return !h_ || h_.done(); }

    bool await_ready() const noexcept { return done(); }

    void await_suspend(std::coroutine_handle<> awaiter) noexcept {
        h_.promise().continuation = awaiter;
    }

    T await_resume() {
        auto& promise = h_.promise();
        if (promise.exception) {
            std::rethrow_exception(promise.exception);
        }
        return std::move(*promise.value);
    }

private:
    std::coroutine_handle<promise_type> h_{};
};

template <> class [[nodiscard]] Task<void> {
public:
    struct promise_type {
        std::exception_ptr exception;
        std::coroutine_handle<> continuation;

        Task get_return_object() {
            return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        std::suspend_never initial_suspend() noexcept { return {}; }
        detail::FinalAwaiter final_suspend() noexcept { return {}; }

        void return_void() noexcept {}

        void unhandled_exception() noexcept { exception = std::current_exception(); }
    };

    Task() noexcept = default;
    explicit Task(std::coroutine_handle<promise_type> handle) noexcept : h_(handle) {}

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;
    // See Task<T> primary template: move-assignment is deleted to avoid
    // tearing down a frame an awaiter's continuation still points at.
    Task(Task&& other) noexcept : h_(std::exchange(other.h_, {})) {}
    Task& operator=(Task&&) = delete;

    ~Task() {
        if (h_) {
            h_.destroy();
        }
    }

    [[nodiscard]] bool done() const noexcept { return !h_ || h_.done(); }

    bool await_ready() const noexcept { return done(); }

    void await_suspend(std::coroutine_handle<> awaiter) noexcept {
        h_.promise().continuation = awaiter;
    }

    void await_resume() {
        if (h_.promise().exception) {
            std::rethrow_exception(h_.promise().exception);
        }
    }

private:
    std::coroutine_handle<promise_type> h_{};
};

// Loop-aware resumption awaiter. `co_await resumeOn(loop)` re-posts
// the awaiting coroutine's continuation onto `loop`, so every statement *after*
// the co_await is guaranteed to execute on that loop. It exists to fix the
// cross-loop-resumption footgun once, for every coroutine: a coroutine that
// suspends on one loop and resumes on a foreign loop (e.g. an upstream
// HttpClient bound to the dedicated domain loop) uses it to hop back onto the
// loop that owns a connection before touching that connection — Drogon's
// response callback and per-connection write buffer must only be touched on
// the loop that accepted the request.
//
// `Loop` is a template parameter precisely so Task.h keeps its <coroutine>-only
// dependency — the contract that lets it cross the plugin ABI without plugins
// linking trantor/Drogon. Daemon code instantiates it with trantor::EventLoop
// (which supplies isInLoopThread() / queueInLoop()); plugins never instantiate
// it, so no trantor type leaks into a plugin .so. A null loop means "no loop to
// hop to" (e.g. a unit test with no running event loop) and resumes inline.
template <class Loop> struct [[nodiscard]] ResumeOnAwaiter {
    Loop* loop;

    [[nodiscard]] bool await_ready() const noexcept {
        return loop == nullptr || loop->isInLoopThread();
    }

    void await_suspend(std::coroutine_handle<> h) const {
        loop->queueInLoop([h]() { h.resume(); });
    }

    void await_resume() const noexcept {}
};

template <class Loop> [[nodiscard]] ResumeOnAwaiter<Loop> resumeOn(Loop* loop) noexcept {
    return ResumeOnAwaiter<Loop>{loop};
}

template <class Loop> [[nodiscard]] ResumeOnAwaiter<Loop> resumeOn(Loop& loop) noexcept {
    return ResumeOnAwaiter<Loop>{&loop};
}

} // namespace aid::plumbing
