#include <arpa/inet.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/HttpTypes.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>

#include "IntegrationHarness.h"
#include "MockHttpServer.h"
#include "aid/controllers/CallController.h"
#include "aid/crosscutting/Clock.h"
#include "aid/crosscutting/CorrelationId.h"
#include "aid/crosscutting/Logger.h"
#include "aid/infrastructure/Mailbox.h"
#include "aid/infrastructure/PluginLoader.h"
#include "aid/infrastructure/Wal.h"
#include "aid/plumbing/Error.h"
#include "aid/plumbing/Result.h"
#include "aid/plumbing/Task.h"
#include "aid/ports/TicketStore.h"
#include "aid/value-types/CallEvent.h"
#include "aid/value-types/Ids.h"

#ifndef AID_OPENPROJECT_PLUGIN_PATH
#error "AID_OPENPROJECT_PLUGIN_PATH must be defined by CMake"
#endif

namespace {

using aid::CallId;
using aid::IncomingCall;
using aid::PhoneNumber;
using aid::controllers::CallController;
using aid::crosscutting::Clock;
using aid::crosscutting::CorrelationId;
using aid::crosscutting::Logger;
using aid::infrastructure::Mailbox;
using aid::infrastructure::Wal;
using aid::plumbing::ErrorCode;
using aid::plumbing::Result;
using aid::plumbing::Task;
using aid::tests::integration::LoggerOnce;
using aid::tests::integration::LoopThread;
using aid::tests::integration::waitUntil;

class FakeClock final : public Clock {
public:
    [[nodiscard]] aid::Timestamp now() const override { return now_; }
    aid::Timestamp now_{};
};

Mailbox::Handlers noopHandlers() {
    auto noop = [](auto&&...) -> Task<Result<void>> { co_return Result<void>{}; };
    return Mailbox::Handlers{noop, noop, noop, noop, noop};
}

// SIGTERM drain semantics. The signal handler + 0.5 s poller + detached
// drain thread in src/main.cpp:563-596 ultimately call Mailbox::drain;
// these tests exercise the drain coroutine directly so the timing/budget
// contract is asserted without forking a
// daemon subprocess.
class SigtermDrainE2E : public ::testing::Test {
protected:
    void SetUp() override {
        const auto pid = static_cast<std::uint64_t>(::getpid());
        static std::atomic<std::uint64_t> counter{0};
        const auto n = counter.fetch_add(1, std::memory_order_relaxed);
        dir_ = std::filesystem::temp_directory_path() /
               ("aid_drain_" + std::to_string(pid) + "_" + std::to_string(n));
        std::filesystem::create_directories(dir_);
        walPath_ = (dir_ / "inbox.log").string();
        wal_ = std::make_unique<Wal>(walPath_, clock_);
    }

    void TearDown() override {
        wal_.reset();
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    static std::string incomingJson(std::string_view callid) {
        std::string out = R"({"event":"Incoming Call","remote":"+491701234567","callid":")";
        out.append(callid);
        out.append(R"(","dialed":"+4930"})");
        return out;
    }

    LoggerOnce loggerInit_{};
    FakeClock clock_;
    CorrelationId cid_;
    std::filesystem::path dir_;
    std::string walPath_;
    std::unique_ptr<Wal> wal_;
};

} // namespace

// Drain waits for an in-flight worker to finish, then returns. The fast
// path: release the latch quickly and assert drain joins shortly after.
TEST_F(SigtermDrainE2E, Drain_CompletesAfterInFlightHandlerFinishes) {
    LoopThread lt;

    auto promise = std::make_shared<std::promise<void>>();
    auto fut = promise->get_future().share();
    std::atomic<int> entered{0};
    std::atomic<int> dispatched{0};

    auto handlers = noopHandlers();
    handlers.incoming = [fut, &entered, &dispatched](const IncomingCall&,
                                                     bool) -> Task<Result<void>> {
        entered.fetch_add(1, std::memory_order_release);
        fut.wait();
        dispatched.fetch_add(1, std::memory_order_relaxed);
        co_return Result<void>{};
    };

    Mailbox mb{lt.loop(), *wal_, Logger::instance(), std::move(handlers), nullptr};

    const CallId hot{"slow"};
    const auto seq = *wal_->append("{}", "cid-d");
    ASSERT_TRUE(mb.enqueue(hot,
                           IncomingCall{hot, PhoneNumber{"+491701234567"}, PhoneNumber{"+4930"}},
                           "cid-d", seq)
                    .has_value());
    ASSERT_TRUE(waitUntil([&] { return entered.load() == 1; }));

    // Mailbox.h forbids drain on a loop thread; this is the non-loop thread.
    std::atomic<bool> drainReturned{false};
    std::thread drainer([&] {
        auto t = mb.drain(std::chrono::seconds{10});
        (void)t;
        drainReturned.store(true, std::memory_order_release);
    });

    // Drain is now spinning on its 10 ms poll loop. Release the handler.
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    EXPECT_FALSE(drainReturned.load()) << "drain must still be polling while a worker is in flight";
    promise->set_value();

    ASSERT_TRUE(waitUntil([&] { return drainReturned.load(); }, std::chrono::milliseconds{2000}))
        << "drain must return once activeWorkers_ goes empty";
    drainer.join();

    EXPECT_EQ(dispatched.load(), 1);
}

// When the handler refuses to finish, drain returns after the budget
// elapses. The worker is left running (and will be reaped by the loop
// thread teardown) — drain's contract is "wait up to budget", not
// "guarantee completion".
TEST_F(SigtermDrainE2E, Drain_ExceedsBudget_ReturnsAfterBudget) {
    LoopThread lt;

    auto promise = std::make_shared<std::promise<void>>();
    auto fut = promise->get_future().share();
    std::atomic<int> entered{0};

    auto handlers = noopHandlers();
    handlers.incoming = [fut, &entered](const IncomingCall&, bool) -> Task<Result<void>> {
        entered.fetch_add(1, std::memory_order_release);
        fut.wait();
        co_return Result<void>{};
    };

    Mailbox mb{lt.loop(), *wal_, Logger::instance(), std::move(handlers), nullptr};

    const CallId hot{"stuck"};
    const auto seq = *wal_->append("{}", "cid-s");
    ASSERT_TRUE(mb.enqueue(hot,
                           IncomingCall{hot, PhoneNumber{"+491701234567"}, PhoneNumber{"+4930"}},
                           "cid-s", seq)
                    .has_value());
    ASSERT_TRUE(waitUntil([&] { return entered.load() == 1; }));

    const auto budget = std::chrono::seconds{1};
    const auto start = std::chrono::steady_clock::now();
    std::thread drainer([&] {
        auto t = mb.drain(budget);
        (void)t;
    });
    drainer.join();
    const auto elapsed = std::chrono::steady_clock::now() - start;

    // Drain polls every 10 ms; the first poll past `deadline` breaks. Allow
    // generous slack so a slow CI host doesn't fail us.
    EXPECT_GE(elapsed, std::chrono::milliseconds{900})
        << "drain must wait approximately its full budget when the handler never finishes";
    EXPECT_LT(elapsed, std::chrono::seconds{3})
        << "drain must respect its budget and not block on the stuck handler";

    // Release so ~Mailbox's two barriers can finish without timing out.
    promise->set_value();
}

// While drain is in progress every new POST to /call must be rejected with
// 503 (the controller maps the "draining" Error to 503 per
// CallController.cpp:160-167).
TEST_F(SigtermDrainE2E, CallController_DuringDrain_Returns503) {
    LoopThread lt;

    auto promise = std::make_shared<std::promise<void>>();
    auto fut = promise->get_future().share();
    std::atomic<int> entered{0};

    auto handlers = noopHandlers();
    handlers.incoming = [fut, &entered](const IncomingCall&, bool) -> Task<Result<void>> {
        entered.fetch_add(1, std::memory_order_release);
        fut.wait();
        co_return Result<void>{};
    };

    Mailbox mb{lt.loop(), *wal_, Logger::instance(), std::move(handlers), nullptr};
    CallController controller{*wal_, mb, Logger::instance(), cid_};

    // Warm up: one POST in flight to keep activeWorkers_ non-empty.
    const std::string body = incomingJson("warm");
    {
        auto req = drogon::HttpRequest::newHttpRequest();
        req->setMethod(drogon::Post);
        req->setBody(body);
        std::optional<drogon::HttpStatusCode> status;
        controller.handlePost(
            req, [&status](const drogon::HttpResponsePtr& r) { status = r->getStatusCode(); });
        ASSERT_TRUE(status.has_value());
        EXPECT_EQ(*status, drogon::k202Accepted);
    }
    ASSERT_TRUE(waitUntil([&] { return entered.load() == 1; }));

    std::thread drainer([&] {
        auto t = mb.drain(std::chrono::seconds{5});
        (void)t;
    });

    // The drain thread takes the draining_ flag synchronously before its
    // first sleep, so this short pause is enough for the flag to be visible
    // to the controller's enqueue call.
    std::this_thread::sleep_for(std::chrono::milliseconds{20});

    {
        auto req = drogon::HttpRequest::newHttpRequest();
        req->setMethod(drogon::Post);
        req->setBody(incomingJson("rejected"));
        std::optional<drogon::HttpStatusCode> status;
        controller.handlePost(
            req, [&status](const drogon::HttpResponsePtr& r) { status = r->getStatusCode(); });
        ASSERT_TRUE(status.has_value());
        EXPECT_EQ(*status, drogon::k503ServiceUnavailable);
    }

    promise->set_value();
    drainer.join();
}

namespace {

using aid::infrastructure::PluginLoader;
using aid::tests::integration::MockHttpServer;
using aid::tests::integration::MockRequest;
using aid::tests::integration::MockResponse;

std::string teardownOpConfig(std::uint16_t port) {
    return std::string{R"({
        "baseUrl": "http://127.0.0.1:)"} +
           std::to_string(port) + std::string{R"(",
        "apiToken": "teardown-token",
        "statusNew": "1", "statusInProgress": "2", "statusClosed": "3",
        "typeCall": "7",
        "projectNames": {},
        "projectWebBaseUrl": "http://127.0.0.1/projects",
        "customFieldIds": {
            "callId": "1", "callerNumber": "2", "calledNumber": "3",
            "callStart": "4", "callEnd": "5", "callLength": "6",
            "callHandler": "7"
        }
    })"};
}

// Drive one resolveUser through the plugin on the loop thread so its HttpClient
// holds live, loop-bound state by the time we tear it down. Result is ignored —
// the point is to give the destructor real cleanup to queue onto the loop.
void warmPluginHttpClient(trantor::EventLoop& loop, aid::ports::TicketStore& store) {
    struct Inflight {
        std::mutex m;
        std::unordered_map<int, std::unique_ptr<Task<void>>> tasks;
    };
    auto inflight = std::make_shared<Inflight>();
    auto prom = std::make_shared<std::promise<void>>();
    auto fut = prom->get_future();
    loop.queueInLoop([&loop, &store, prom, inflight] {
        auto coro = [](aid::ports::TicketStore* s, std::shared_ptr<std::promise<void>> p,
                       std::shared_ptr<Inflight> inf, trantor::EventLoop* lp) -> Task<void> {
            try {
                (void)co_await s->resolveUser("alice");
            } catch (...) {
                // ignored — we only need the round-trip's side effects on the client
            }
            p->set_value();
            lp->queueInLoop([inf] {
                std::lock_guard lk{inf->m};
                inf->tasks.clear();
            });
        }(&store, prom, inflight, &loop);
        std::lock_guard lk{inflight->m};
        inflight->tasks.emplace(0, std::make_unique<Task<void>>(std::move(coro)));
    });
    fut.wait();
}

} // namespace

// Teardown ordering. src/main.cpp releases the plugins (running destroy_*,
// whose HttpClient destructors queue cleanup onto the domain loop) WHILE that
// loop is still alive, and only then lets DomainLoop's RAII stop it
// (DomainLoop must outlive plugins). This
// reproduces that exact order with the real .so on a live loop and asserts a
// clean teardown — under ASan/UBSan a use-after-free here (the failure mode the
// ordering prevents: enqueue onto a freed EventLoop) aborts the test.
//
// The FULL signal→drain→quit→join→release→unwind sequence is main()-composition
// only: it threads the global drogon::app() loop, a detached drain thread and
// stack-unwind order, none of which is reachable without forking a real daemon
// (which would need the live OpenProject/DaviCal/auth.db env and is therefore
// not CI-reproducible). The load-bearing, regression-prone step — release the
// plugin before the loop dies — is the part this test pins down.
TEST(SigtermTeardownOrdering, RealPluginReleasedWhileLoopAlive_NoUseAfterFree) {
    LoggerOnce loggerInit;
    MockHttpServer op([](const MockRequest&) {
        return MockResponse{200, "OK", "application/json",
                            R"({"_embedded":{"elements":[{"login":"alice","id":9,)"
                            R"("_links":{"self":{"href":"/api/v3/users/9"}}}]}})"};
    });

    auto lt = std::make_unique<LoopThread>();
    PluginLoader<aid::ports::TicketStore> loader;
    const auto loaded = loader.loadWithLoop(AID_OPENPROJECT_PLUGIN_PATH, "create_TicketStore",
                                            "destroy_TicketStore", teardownOpConfig(op.port()),
                                            &lt->loop(), ::geteuid());
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;
    ASSERT_NE(loader.get(), nullptr);

    warmPluginHttpClient(lt->loop(), *loader.get());

    // main()'s order: destroy the plugin first, WHILE the loop still runs...
    loader.releaseInstance();
    EXPECT_EQ(loader.get(), nullptr);

    // ...then stop the loop (LoopThread's dtor). Reversing these two lines is
    // the use-after-free this ordering exists to prevent.
    lt.reset();
}

// The cancellation fix proved THROUGH THE REAL PLUGIN.
// A request is held genuinely in flight (the mock server receives it but blocks
// before replying), so the plugin's worker coroutine is suspended inside an
// OpenProject co_await. The port's cancelPendingRequests() hook — the exact call
// main() makes during the graceful drain — must resume that coroutine PROMPTLY
// with an error, instead of waiting out the 30 s read timeout. This is the
// deterministic stand-in for the live forked-daemon path: it pins the mechanism
// that stops a mailbox worker from being destroyed while still suspended in HTTP,
// and (unlike the earlier ~HttpClient attempt) it does so while the plugin is
// fully alive — resuming after release would run the chain through freed plugin
// internals (the shutdown SIGSEGV this redesign fixes).
TEST(SigtermTeardownOrdering, CancelPendingRequestsResumesInflightThroughRealPlugin) {
    LoggerOnce loggerInit;

    // Responder blocks until the test releases it: the request reaches the server
    // (so the plugin is provably suspended in HTTP) but never gets a reply on its
    // own — only the cancel can resolve it.
    auto release = std::make_shared<std::promise<void>>();
    auto releaseFut = release->get_future().share();
    std::atomic<int> hits{0};
    MockHttpServer op([&hits, releaseFut](const MockRequest&) {
        hits.fetch_add(1, std::memory_order_release);
        releaseFut.wait();
        return MockResponse{200, "OK", "application/json",
                            R"({"_embedded":{"elements":[{"login":"alice","id":9,)"
                            R"("_links":{"self":{"href":"/api/v3/users/9"}}}]}})"};
    });

    auto lt = std::make_unique<LoopThread>();
    PluginLoader<aid::ports::TicketStore> loader;
    const auto loaded = loader.loadWithLoop(AID_OPENPROJECT_PLUGIN_PATH, "create_TicketStore",
                                            "destroy_TicketStore", teardownOpConfig(op.port()),
                                            &lt->loop(), ::geteuid());
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;
    auto* store = loader.get();
    ASSERT_NE(store, nullptr);

    // Launch resolveUser on the loop, non-blocking; record whether it returned an
    // Error (the cancelled outcome). The Task<void> frame lives in `holder` until
    // it self-clears on the loop after completing.
    struct Holder {
        std::mutex m;
        std::unique_ptr<Task<void>> task;
    };
    auto holder = std::make_shared<Holder>();
    auto done = std::make_shared<std::promise<bool>>();
    auto doneFut = done->get_future();
    lt->loop().queueInLoop([&lt, store, done, holder] {
        auto coro = [](aid::ports::TicketStore* s, std::shared_ptr<std::promise<bool>> d,
                       std::shared_ptr<Holder> h, trantor::EventLoop* lp) -> Task<void> {
            auto r = co_await s->resolveUser("alice");
            d->set_value(!r.has_value()); // cancelled → Error → true
            lp->queueInLoop([h] {
                std::lock_guard lk{h->m};
                h->task.reset();
            });
        }(store, done, holder, &lt->loop());
        std::lock_guard lk{holder->m};
        holder->task = std::make_unique<Task<void>>(std::move(coro));
    });

    // The request is in flight once the server has received it.
    ASSERT_TRUE(waitUntil([&] { return hits.load(std::memory_order_acquire) >= 1; }));
    EXPECT_NE(doneFut.wait_for(std::chrono::milliseconds{150}), std::future_status::ready)
        << "resolveUser must still be suspended in HTTP (responder is blocked)";

    // The port shutdown hook resumes the suspended coroutine at once.
    store->cancelPendingRequests();
    ASSERT_EQ(doneFut.wait_for(std::chrono::seconds{3}), std::future_status::ready)
        << "cancelPendingRequests must resume the in-flight request, not wait out readTimeout";
    EXPECT_TRUE(doneFut.get()) << "a cancelled resolveUser should surface an Error";

    // Release the blocked responder so the mock replies to the (already
    // cancelled) request and closes the socket; the late drogon completion is a
    // no-op (settled guard). Wait until the server side has fully finished that
    // connection so the loop's teardown poll reclaims the drogon connection
    // rather than leaking it under LSan; also confirm the coroutine frame is
    // gone.
    release->set_value();
    ASSERT_TRUE(waitUntil([&] { return op.connectionsCompleted() >= 1; }));
    ASSERT_TRUE(waitUntil([&] {
        std::lock_guard lk{holder->m};
        return holder->task == nullptr;
    }));

    // Tear down in main()'s order (plugin released while the loop is alive) —
    // clean under ASan.
    loader.releaseInstance();
    lt.reset();
}

// ===========================================================================
// Forked-real-daemon SIGTERM end-to-end test.
//
// Everything above pins the drain SEMANTICS and the load-bearing teardown step
// at unit level. This test closes the remaining gap: the literal
// signal → drain → quit → join → release → unwind path of the real daemon
// PROCESS. It forks `build/src/aid <config>`, waits for it to bind its
// listener, sends SIGTERM, and asserts a clean exit(0). It also exercises the
// cancellation fix in situ — any plugin HttpClient request still in flight at shutdown is
// cancelled before frames are destroyed, so a clean exit means the teardown
// ordering held end to end.
//
// It is GATED twice over because it needs the live dev environment
// (OpenProject/DaviCal/auth.db — see the running_daemon_live notes) and is not
// CI-reproducible: (1) it self-skips unless AID_LIVE_E2E=1; (2) it self-skips
// if the daemon binary or the config file is absent. Run it deliberately with
// `ctest -L live` (the CMake entry sets AID_LIVE_E2E=1 for you).
//   Env knobs: AID_LIVE_CONFIG (default ~/aid-dev/etc/config.json),
//              AID_LIVE_HEALTH_PORT (default 8088, the dev listenPort).
namespace {

[[nodiscard]] bool probeHealth(std::uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return false;
    }
    ::sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bool ok = false;
    if (::connect(fd, reinterpret_cast<::sockaddr*>(&addr), sizeof(addr)) == 0) {
        const std::string req = "GET /health HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n";
        if (::write(fd, req.data(), req.size()) == static_cast<ssize_t>(req.size())) {
            char buf[64] = {};
            const ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
            // Any well-formed HTTP status line means the listener is up and
            // serving — readiness, independent of upstream health (200 vs 503).
            ok = n > 0 && std::string_view{buf, static_cast<std::size_t>(n)}.find("HTTP/1.") !=
                              std::string_view::npos;
        }
    }
    ::close(fd);
    return ok;
}

[[nodiscard]] bool waitForReady(std::uint16_t port, std::chrono::seconds budget) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        if (probeHealth(port)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }
    return false;
}

// waitpid with a wall-clock budget. Returns true if the child reaped within
// budget (status filled), false on timeout.
[[nodiscard]] bool waitForExit(::pid_t pid, int* status, std::chrono::seconds budget) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        const ::pid_t r = ::waitpid(pid, status, WNOHANG);
        if (r == pid) {
            return true;
        }
        if (r < 0) {
            return false; // already reaped / error
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }
    return false;
}

[[nodiscard]] std::string liveConfigPath() {
    if (const char* c = std::getenv("AID_LIVE_CONFIG"); c != nullptr && *c != '\0') {
        return c;
    }
    const char* home = std::getenv("HOME");
    return std::string{home != nullptr ? home : ""} + "/aid-dev/etc/config.json";
}

[[nodiscard]] std::uint16_t liveHealthPort() {
    if (const char* p = std::getenv("AID_LIVE_HEALTH_PORT"); p != nullptr && *p != '\0') {
        return static_cast<std::uint16_t>(std::atoi(p));
    }
    return 8088;
}

} // namespace

TEST(SigtermForkE2E, ForkedDaemonDrainsAndExitsZeroOnSigterm) {
    if (const char* gate = std::getenv("AID_LIVE_E2E");
        gate == nullptr || std::string_view{gate} != "1") {
        GTEST_SKIP() << "live forked-daemon E2E is gated off — set AID_LIVE_E2E=1 (needs the dev "
                        "env: OpenProject/DaviCal/auth.db, see running_daemon_live). "
                        "Run via `ctest -L live`.";
    }

    const std::string config = liveConfigPath();
    if (!std::filesystem::exists(AID_DAEMON_BINARY_PATH)) {
        GTEST_SKIP() << "daemon binary missing: " << AID_DAEMON_BINARY_PATH;
    }
    if (!std::filesystem::exists(config)) {
        GTEST_SKIP() << "live config missing: " << config
                     << " (set AID_LIVE_CONFIG or scaffold ~/aid-dev per reference_dev_root)";
    }
    const std::uint16_t port = liveHealthPort();

    const ::pid_t pid = ::fork();
    ASSERT_GE(pid, 0) << "fork failed: " << std::strerror(errno);

    if (pid == 0) {
        // Child: become the daemon. execl replaces the image, so none of the
        // test's state (gtest, signal handlers) survives into the daemon.
        ::execl(AID_DAEMON_BINARY_PATH, AID_DAEMON_BINARY_PATH, config.c_str(),
                static_cast<char*>(nullptr));
        ::_exit(127); // exec failed
    }

    // Parent. Guarantee the child is never left running, whatever happens.
    bool reaped = false;
    int status = 0;
    const auto reap = [&](std::chrono::seconds budget) {
        if (reaped) {
            return;
        }
        if (waitForExit(pid, &status, budget)) {
            reaped = true;
        }
    };
    struct Guard {
        ::pid_t pid;
        bool* reaped;
        ~Guard() {
            if (!*reaped) {
                ::kill(pid, SIGKILL);
                int s = 0;
                ::waitpid(pid, &s, 0);
            }
        }
    } guard{pid, &reaped};

    // 1. Wait for the daemon to bind its listener (full startup: plugins,
    //    WAL replay, cold-start ping). 30 s is generous for a healthy box.
    if (!waitForReady(port, std::chrono::seconds{30})) {
        ::kill(pid, SIGKILL);
        reap(std::chrono::seconds{5});
        FAIL() << "daemon never became ready on 127.0.0.1:" << port
               << " — check the dev config / OpenProject docker. (exec result: "
               << (reaped && WIFEXITED(status) && WEXITSTATUS(status) == 127
                       ? "exec failed — bad binary path"
                       : "listener never came up")
               << ")";
    }

    // 2. SIGTERM → the daemon's signal handler arms the drain; the loop timer
    //    drains both mailboxes (≤10 s each) then quit()s and main() unwinds.
    ASSERT_EQ(::kill(pid, SIGTERM), 0) << "kill(SIGTERM) failed: " << std::strerror(errno);

    // 3. Clean exit within the drain budget + margin (10 s call drain + 10 s
    //    webhook drain + teardown). 45 s leaves comfortable slack.
    reap(std::chrono::seconds{45});
    ASSERT_TRUE(reaped) << "daemon did not exit within 45 s of SIGTERM (drain wedged?)";
    ASSERT_TRUE(WIFEXITED(status))
        << "daemon was killed by a signal, not a clean exit"
        << (WIFSIGNALED(status) ? " (signal " + std::to_string(WTERMSIG(status)) + ")" : "");
    EXPECT_EQ(WEXITSTATUS(status), 0) << "daemon exit code should be 0 after a graceful drain";
}
