#include <gtest/gtest.h>

#include <chrono>
#include <nlohmann/json.hpp>
#include <string>

#include "aid/adapters/openproject/internal/OpHttp.h"
#include "aid/plumbing/Error.h"
#include "tests/adapters/openproject_plugin/fake_http_dispatcher.h"

using aid::adapters::openproject::OpHttp;
using aid::plumbing::ErrorCode;
using aid::test_support::FakeHttpDispatcher;
using aid::test_support::FakeSleeper;
using namespace std::chrono_literals;
using json = nlohmann::json;

namespace {

// Helper: drain a synchronous Task<Result<json>> built by OpHttp. The
// Task type uses initial_suspend=suspend_never, so by the time the
// `get`/`post`/`patch` coroutine returns, it has run to completion (the
// only co_await inside hits the FakeHttpDispatcher, which is itself
// synchronous). done() must be true; await_resume yields the value.
template <class T> auto drainSync(aid::plumbing::Task<T>&& task) {
    EXPECT_TRUE(task.done());
    return task.await_resume();
}

} // namespace

TEST(OpHttp, GetReturnsParsedJsonOn2xx) {
    FakeHttpDispatcher d;
    FakeSleeper s;
    d.enqueueResponse(200, R"({"hello":"world"})");
    OpHttp http(d, "http://op.example.com", "t", s.sleeper());

    auto result = drainSync(http.get("/api/v3/x"));
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ((*result)["hello"], "world");

    ASSERT_EQ(d.calls().size(), 1U);
    EXPECT_EQ(d.calls()[0].method, "GET");
    EXPECT_EQ(d.calls()[0].path, "/api/v3/x");
}

TEST(OpHttp, AuthorizationHeaderIsBase64ApiKey) {
    FakeHttpDispatcher d;
    FakeSleeper s;
    d.enqueueResponse(200, "{}");
    OpHttp http(d, "http://op.example.com", "secret", s.sleeper());

    (void)drainSync(http.get("/x"));
    ASSERT_EQ(d.calls().size(), 1U);

    // "apikey:secret" base64 = "YXBpa2V5OnNlY3JldA=="
    bool foundAuth = false;
    for (const auto& [k, v] : d.calls()[0].headers) {
        if (k == "Authorization") {
            EXPECT_EQ(v, "Basic YXBpa2V5OnNlY3JldA==");
            foundAuth = true;
        }
    }
    EXPECT_TRUE(foundAuth);
}

TEST(OpHttp, NonJsonBodyOn200IsAnError) {
    FakeHttpDispatcher d;
    FakeSleeper s;
    d.enqueueResponse(200, "not json");
    OpHttp http(d, "http://op.example.com", "t", s.sleeper());

    auto result = drainSync(http.get("/x"));
    ASSERT_FALSE(result.has_value());
}

TEST(OpHttp, FourOhFourMapsToNotFound) {
    FakeHttpDispatcher d;
    FakeSleeper s;
    d.enqueueResponse(404, "{}");
    OpHttp http(d, "http://op.example.com", "t", s.sleeper());

    auto result = drainSync(http.get("/x"));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::NotFound);
}

TEST(OpHttp, FourOhOneMapsToUnauthenticated) {
    FakeHttpDispatcher d;
    FakeSleeper s;
    d.enqueueResponse(401, "{}");
    OpHttp http(d, "http://op.example.com", "t", s.sleeper());

    auto result = drainSync(http.get("/x"));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::Unauthenticated);
}

TEST(OpHttp, FourOhNineMapsToConflict409) {
    FakeHttpDispatcher d;
    FakeSleeper s;
    d.enqueueResponse(409, R"({"_type":"Error"})");
    OpHttp http(d, "http://op.example.com", "t", s.sleeper());

    auto result = drainSync(http.patch("/x", json::object()));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::Conflict409);
}

TEST(OpHttp, FiveHundredMapsToUpstreamUnavailable) {
    FakeHttpDispatcher d;
    FakeSleeper s;
    d.enqueueResponse(500, "boom");
    OpHttp http(d, "http://op.example.com", "t", s.sleeper());

    auto result = drainSync(http.get("/x"));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::UpstreamUnavailable);
}

TEST(OpHttp, PostSerializesJsonBodyAndAddsContentType) {
    FakeHttpDispatcher d;
    FakeSleeper s;
    d.enqueueResponse(201, "{}");
    OpHttp http(d, "http://op.example.com", "t", s.sleeper());

    json body = {{"a", 1}, {"b", "two"}};
    (void)drainSync(http.post("/x", body));

    ASSERT_EQ(d.calls().size(), 1U);
    EXPECT_EQ(d.calls()[0].method, "POST");
    EXPECT_EQ(json::parse(d.calls()[0].body), body);

    bool foundCt = false;
    for (const auto& [k, v] : d.calls()[0].headers) {
        if (k == "Content-Type" && v == "application/json")
            foundCt = true;
    }
    EXPECT_TRUE(foundCt);
}

// ─── retryOn409 — backoff ─────────────────────────────────────────────────

namespace {

// Test helper: a patchFn that returns scripted ordered (Conflict409|OK)
// results. Useful for asserting exact retry counts.
struct ScriptedPatcher {
    int callCount = 0;
    std::vector<aid::plumbing::Result<json>> results;

    aid::plumbing::Task<aid::plumbing::Result<json>> operator()() {
        ++callCount;
        if (results.empty()) {
            co_return aid::plumbing::unexpected{aid::plumbing::Error{
                aid::plumbing::ErrorCode::Unknown, "no more scripted results", std::nullopt}};
        }
        auto r = std::move(results.front());
        results.erase(results.begin());
        co_return r;
    }
};

struct CountingRefresh {
    int callCount = 0;
    aid::plumbing::Task<aid::plumbing::Result<int>> operator()() {
        ++callCount;
        co_return 42;
    }
};

aid::plumbing::Result<json> conflict() {
    return aid::plumbing::unexpected{
        aid::plumbing::Error{ErrorCode::Conflict409, "stale", std::nullopt}};
}

} // namespace

TEST(OpHttp, RetryOn409SucceedsOnFirstAttemptIfNoConflict) {
    FakeHttpDispatcher d;
    FakeSleeper s;
    OpHttp http(d, "http://op.example.com", "t", s.sleeper());

    ScriptedPatcher patcher;
    patcher.results.push_back(json::object());
    CountingRefresh refresh;

    auto task = http.retryOn409([&]() { return patcher(); }, [&]() { return refresh(); });
    auto result = drainSync(std::move(task));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(patcher.callCount, 1);
    EXPECT_EQ(refresh.callCount, 0);
    EXPECT_TRUE(s.durations.empty());
}

TEST(OpHttp, RetryOn409SleepsAfterEachConflictUpToFive) {
    FakeHttpDispatcher d;
    FakeSleeper s;
    OpHttp http(d, "http://op.example.com", "t", s.sleeper());

    ScriptedPatcher patcher;
    // Two conflicts then a success — third attempt wins.
    patcher.results.push_back(conflict());
    patcher.results.push_back(conflict());
    patcher.results.push_back(json::object());
    CountingRefresh refresh;

    auto task = http.retryOn409([&]() { return patcher(); }, [&]() { return refresh(); });
    auto result = drainSync(std::move(task));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(patcher.callCount, 3);
    EXPECT_EQ(refresh.callCount, 2);
    ASSERT_EQ(s.durations.size(), 2U);
    EXPECT_EQ(s.durations[0], 50ms);
    EXPECT_EQ(s.durations[1], 100ms);
}

TEST(OpHttp, RetryOn409ExhaustsAfterFiveConflictsWithFullBackoffLadder) {
    FakeHttpDispatcher d;
    FakeSleeper s;
    OpHttp http(d, "http://op.example.com", "t", s.sleeper());

    ScriptedPatcher patcher;
    for (int i = 0; i < 5; ++i)
        patcher.results.push_back(conflict());
    CountingRefresh refresh;

    auto task = http.retryOn409([&]() { return patcher(); }, [&]() { return refresh(); });
    auto result = drainSync(std::move(task));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::LockVersionExhausted);
    EXPECT_EQ(patcher.callCount, 5);
    EXPECT_EQ(refresh.callCount, 5);
    ASSERT_EQ(s.durations.size(), 5U);
    EXPECT_EQ(s.durations[0], 50ms);
    EXPECT_EQ(s.durations[1], 100ms);
    EXPECT_EQ(s.durations[2], 200ms);
    EXPECT_EQ(s.durations[3], 400ms);
    EXPECT_EQ(s.durations[4], 800ms);
}

TEST(OpHttp, RetryOn409ShortCircuitsOnNon409Error) {
    FakeHttpDispatcher d;
    FakeSleeper s;
    OpHttp http(d, "http://op.example.com", "t", s.sleeper());

    ScriptedPatcher patcher;
    patcher.results.push_back(aid::plumbing::unexpected{
        aid::plumbing::Error{ErrorCode::UpstreamUnavailable, "503", std::nullopt}});
    CountingRefresh refresh;

    auto task = http.retryOn409([&]() { return patcher(); }, [&]() { return refresh(); });
    auto result = drainSync(std::move(task));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::UpstreamUnavailable);
    EXPECT_EQ(patcher.callCount, 1);
    EXPECT_EQ(refresh.callCount, 0); // never refreshed
    EXPECT_TRUE(s.durations.empty());
}

TEST(OpHttp, RetryOn409StopsIfRefreshLockVersionFails) {
    FakeHttpDispatcher d;
    FakeSleeper s;
    OpHttp http(d, "http://op.example.com", "t", s.sleeper());

    ScriptedPatcher patcher;
    patcher.results.push_back(conflict());

    struct FailingRefresh {
        int callCount = 0;
        aid::plumbing::Task<aid::plumbing::Result<int>> operator()() {
            ++callCount;
            co_return aid::plumbing::unexpected{aid::plumbing::Error{
                ErrorCode::NotFound, "ticket vanished mid-retry", std::nullopt}};
        }
    } refresh;

    auto task = http.retryOn409([&]() { return patcher(); }, [&]() { return refresh(); });
    auto result = drainSync(std::move(task));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::NotFound);
    EXPECT_EQ(patcher.callCount, 1);
    EXPECT_EQ(refresh.callCount, 1);
    // The retry loop sleeps BEFORE refreshing, so one backoff lands even when the
    // very next refresh fails. (See OpHttp::retryOn409 — sleep is
    // co_awaited before refreshLockVersion in the catch handler.)
    ASSERT_EQ(s.durations.size(), 1U);
    EXPECT_EQ(s.durations[0], 50ms);
}
