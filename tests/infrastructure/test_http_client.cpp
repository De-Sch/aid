#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <trantor/net/EventLoop.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "aid/infrastructure/HttpClient.h"
#include "aid/plumbing/Error.h"
#include "aid/plumbing/Result.h"
#include "aid/plumbing/Task.h"

namespace {

using aid::infrastructure::Headers;
using aid::infrastructure::HttpClient;
using aid::infrastructure::HttpResponse;
using aid::infrastructure::UpstreamConfig;
using aid::plumbing::ErrorCode;
using aid::plumbing::Result;
using aid::plumbing::Task;

// Mirrors MailboxTest::LoopThread — own EventLoop on a dedicated thread.
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

// Raw-socket HTTP/1.1 server. Each connection: read until \r\n\r\n (plus any
// Content-Length body), record the request, then act per `Behavior`.
// Deliberately minimal — no Trantor TcpServer because we need the actual
// bound port (Trantor doesn't expose it on a port-0 bind) and we need
// per-connection behavior changes (e.g. RetryThenSuccess).
class FakeServer {
public:
    enum class Behavior {
        Reply,              // write `canned` then close
        CloseImmediately,   // close before reading; triggers NetworkFailure
        AcceptThenHang,     // read request, then never write; triggers Timeout
        AcceptThenHangLong, // read request, then hang (interruptibly) ~30 s; only
                            // a client-side cancel or readTimeout resolves it
    };

    explicit FakeServer(std::string canned = defaultOk(), Behavior initial = Behavior::Reply)
        : canned_(std::move(canned)), defaultBehavior_(initial) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) {
            ADD_FAILURE() << "socket: " << std::strerror(errno);
            return;
        }
        int yes = 1;
        ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        ::sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(fd_, reinterpret_cast<::sockaddr*>(&addr), sizeof(addr)) != 0) {
            ADD_FAILURE() << "bind: " << std::strerror(errno);
            return;
        }

        ::socklen_t len = sizeof(addr);
        if (::getsockname(fd_, reinterpret_cast<::sockaddr*>(&addr), &len) != 0) {
            ADD_FAILURE() << "getsockname: " << std::strerror(errno);
            return;
        }
        port_ = ntohs(addr.sin_port);

        if (::listen(fd_, 8) != 0) {
            ADD_FAILURE() << "listen: " << std::strerror(errno);
            return;
        }

        thread_ = std::thread([this] { serve(); });
    }

    FakeServer(const FakeServer&) = delete;
    FakeServer& operator=(const FakeServer&) = delete;
    FakeServer(FakeServer&&) = delete;
    FakeServer& operator=(FakeServer&&) = delete;

    ~FakeServer() {
        stop_.store(true, std::memory_order_release);
        if (fd_ >= 0) {
            ::shutdown(fd_, SHUT_RDWR);
            ::close(fd_);
            fd_ = -1;
        }
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

    [[nodiscard]] int connectionCount() const noexcept {
        return connectionCount_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::string lastRequest() const {
        std::lock_guard lk{mtx_};
        return lastRequest_;
    }

    // Queue per-connection behavior overrides (consumed in FIFO order; once
    // empty, falls back to defaultBehavior_).
    void queueBehavior(Behavior b) {
        std::lock_guard lk{mtx_};
        queued_.push_back(b);
    }

    static std::string defaultOk() {
        return "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nConnection: close\r\n\r\nhello";
    }

    static std::string with(int status, std::string statusText, std::string body) {
        std::string out = "HTTP/1.1 " + std::to_string(status) + " " + statusText +
                          "\r\nContent-Length: " + std::to_string(body.size()) +
                          "\r\nConnection: close\r\n\r\n" + body;
        return out;
    }

private:
    void serve() {
        while (!stop_.load(std::memory_order_acquire)) {
            int client = ::accept(fd_, nullptr, nullptr);
            if (client < 0) {
                return; // listen socket shut down
            }
            connectionCount_.fetch_add(1, std::memory_order_acq_rel);
            Behavior b = defaultBehavior_;
            {
                std::lock_guard lk{mtx_};
                if (!queued_.empty()) {
                    b = queued_.front();
                    queued_.erase(queued_.begin());
                }
            }
            handle(client, b);
            ::close(client);
        }
    }

    void handle(int client, Behavior b) {
        if (b == Behavior::CloseImmediately) {
            return;
        }
        std::string req;
        char buf[2048];
        while (req.find("\r\n\r\n") == std::string::npos) {
            const ssize_t n = ::read(client, buf, sizeof(buf));
            if (n <= 0) {
                return;
            }
            req.append(buf, static_cast<std::size_t>(n));
        }

        // Drogon normalizes header names to lowercase on the wire; search the
        // headers case-insensitively for content-length: <N>. If found, keep
        // reading until we have N bytes of body past the \r\n\r\n.
        const auto headerEnd = req.find("\r\n\r\n");
        std::string lowerHeaders;
        lowerHeaders.reserve(headerEnd);
        for (std::size_t i = 0; i < headerEnd; ++i) {
            lowerHeaders.push_back(
                static_cast<char>(::tolower(static_cast<unsigned char>(req[i]))));
        }
        const auto clPos = lowerHeaders.find("content-length:");
        if (clPos != std::string::npos) {
            const auto valStart = clPos + std::string{"content-length:"}.size();
            const auto eol = lowerHeaders.find("\r\n", valStart);
            const auto valStr =
                lowerHeaders.substr(valStart, eol == std::string::npos ? eol : eol - valStart);
            const std::size_t contentLength = static_cast<std::size_t>(std::stoul(valStr));
            const std::size_t bodyStart = headerEnd + 4;
            while (req.size() < bodyStart + contentLength) {
                const ssize_t n = ::read(client, buf, sizeof(buf));
                if (n <= 0) {
                    break;
                }
                req.append(buf, static_cast<std::size_t>(n));
            }
        }

        {
            std::lock_guard lk{mtx_};
            lastRequest_ = req;
        }
        if (b == Behavior::AcceptThenHang) {
            // Sleep up to a second, then close. The client must time out
            // before this returns — tests set readTimeout well below 1 s.
            std::this_thread::sleep_for(std::chrono::seconds{1});
            return;
        }
        if (b == Behavior::AcceptThenHangLong) {
            // Hang for ~30 s but poll stop_ every 50 ms so the dtor's join()
            // returns promptly once the test is done. The request resolves only
            // via a client-side cancel (or a readTimeout the test sets high).
            for (int i = 0; i < 600 && !stop_.load(std::memory_order_acquire); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds{50});
            }
            return;
        }
        const ssize_t w = ::write(client, canned_.data(), canned_.size());
        (void)w;
    }

    std::string canned_;
    Behavior defaultBehavior_;
    std::thread thread_;
    int fd_{-1};
    std::uint16_t port_{0};
    std::atomic<bool> stop_{false};
    std::atomic<int> connectionCount_{0};
    mutable std::mutex mtx_;
    std::string lastRequest_;
    std::vector<Behavior> queued_;
};

class HttpClientTest : public ::testing::Test {
protected:
    void TearDown() override {
        // Drain any leftover in-flight task holders so ASan sees clean teardown.
        std::lock_guard lk{inFlightMtx_};
        inFlight_.clear();
    }

    // Submits a Task-producing factory to the loop, blocks until done, returns
    // the result. The factory runs ON the loop thread (the same thread the
    // HttpClient is pinned to), so the awaiter chain resumes coherently.
    template <class T> T runOnLoop(std::function<Task<Result<T>>()> factory) {
        auto p = std::make_shared<std::promise<Result<T>>>();
        auto fut = p->get_future();
        const int slot = nextSlot_.fetch_add(1, std::memory_order_relaxed);

        lt_.loop().queueInLoop([this, factory = std::move(factory), p, slot]() mutable {
            auto coro = [](std::function<Task<Result<T>>()> f,
                           std::shared_ptr<std::promise<Result<T>>> prom, int slotId,
                           HttpClientTest* self) -> Task<void> {
                try {
                    Result<T> r = co_await f();
                    prom->set_value(std::move(r));
                } catch (...) {
                    prom->set_exception(std::current_exception());
                }
                // Drop our entry from the in-flight map on the loop thread
                // so the Task<void> holding this frame is destroyed after
                // we've already finished running. Queueing through the loop
                // avoids destroying our own frame.
                self->lt_.loop().queueInLoop([self, slotId] {
                    std::lock_guard lk{self->inFlightMtx_};
                    self->inFlight_.erase(slotId);
                });
            }(std::move(factory), p, slot, this);

            std::lock_guard lk{inFlightMtx_};
            inFlight_.emplace(slot, std::make_unique<Task<void>>(std::move(coro)));
        });

        Result<T> r = fut.get();
        if (r.has_value()) {
            return std::move(*r);
        }
        ADD_FAILURE() << "HttpClient call returned Error: " << static_cast<int>(r.error().code)
                      << " — " << r.error().message;
        // Construct a default-zero HttpResponse for the test to log past.
        return T{};
    }

    // Same as runOnLoop but returns the full Result<T>, including error path.
    template <class T> Result<T> runResult(std::function<Task<Result<T>>()> factory) {
        return launch<T>(std::move(factory)).get();
    }

    // Submits the factory to the loop and returns its future WITHOUT blocking —
    // for tests that must interact with the request (e.g. cancel it) while it is
    // still in flight. The Task<void> holding the frame lives in inFlight_ until
    // it self-erases on the loop, so the future stays valid until consumed.
    template <class T> std::future<Result<T>> launch(std::function<Task<Result<T>>()> factory) {
        auto p = std::make_shared<std::promise<Result<T>>>();
        auto fut = p->get_future();
        const int slot = nextSlot_.fetch_add(1, std::memory_order_relaxed);

        lt_.loop().queueInLoop([this, factory = std::move(factory), p, slot]() mutable {
            auto coro = [](std::function<Task<Result<T>>()> f,
                           std::shared_ptr<std::promise<Result<T>>> prom, int slotId,
                           HttpClientTest* self) -> Task<void> {
                try {
                    Result<T> r = co_await f();
                    prom->set_value(std::move(r));
                } catch (...) {
                    prom->set_exception(std::current_exception());
                }
                self->lt_.loop().queueInLoop([self, slotId] {
                    std::lock_guard lk{self->inFlightMtx_};
                    self->inFlight_.erase(slotId);
                });
            }(std::move(factory), p, slot, this);

            std::lock_guard lk{inFlightMtx_};
            inFlight_.emplace(slot, std::make_unique<Task<void>>(std::move(coro)));
        });

        return fut;
    }

    // Poll `pred` up to `budget` (in 5 ms steps). Returns true once it holds.
    template <class Pred>
    static bool waitFor(Pred pred, std::chrono::milliseconds budget = std::chrono::seconds{2}) {
        const auto deadline = std::chrono::steady_clock::now() + budget;
        while (std::chrono::steady_clock::now() < deadline) {
            if (pred()) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
        return pred();
    }

    [[nodiscard]] std::string baseUrl(std::uint16_t port) const {
        return "http://127.0.0.1:" + std::to_string(port);
    }

    LoopThread lt_{};
    std::mutex inFlightMtx_;
    std::unordered_map<int, std::unique_ptr<Task<void>>> inFlight_;
    std::atomic<int> nextSlot_{0};
};

TEST_F(HttpClientTest, GetReturnsStatusAndBody) {
    FakeServer srv{};
    HttpClient cli{baseUrl(srv.port()), UpstreamConfig{}, lt_.loop()};
    auto r = runResult<HttpResponse>([&] { return cli.get("/hello", Headers{}); });
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->status, 200);
    EXPECT_EQ(r->body, "hello");
}

TEST_F(HttpClientTest, Returns404AsOkWithStatus) {
    FakeServer srv{FakeServer::with(404, "Not Found", "missing")};
    HttpClient cli{baseUrl(srv.port()), UpstreamConfig{}, lt_.loop()};
    auto r = runResult<HttpResponse>([&] { return cli.get("/missing", Headers{}); });
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->status, 404);
    EXPECT_EQ(r->body, "missing");
}

// ASCII lowercase for case-insensitive substring matches. Drogon's wire format
// uses lowercase header names; tests must not be sensitive to that detail.
[[nodiscard]] static std::string toLower(std::string s) {
    for (auto& c : s) {
        c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

TEST_F(HttpClientTest, PostSendsBodyAndContentLength) {
    FakeServer srv{};
    HttpClient cli{baseUrl(srv.port()), UpstreamConfig{}, lt_.loop()};
    const std::string body = R"({"hi":"there"})";
    auto r = runResult<HttpResponse>([&] { return cli.post("/echo", body, Headers{}); });
    ASSERT_TRUE(r.has_value()) << r.error().message;
    const auto seen = srv.lastRequest();
    const auto lower = toLower(seen);
    EXPECT_NE(seen.find(body), std::string::npos) << "body missing in: [" << seen << "]";
    EXPECT_NE(lower.find("content-length: " + std::to_string(body.size())), std::string::npos)
        << "content-length missing in: [" << seen << "]";
    EXPECT_NE(seen.find("POST /echo"), std::string::npos);
}

TEST_F(HttpClientTest, HeadersPropagateToRequest) {
    FakeServer srv{};
    HttpClient cli{baseUrl(srv.port()), UpstreamConfig{}, lt_.loop()};
    Headers h;
    h.kv.emplace_back("X-Foo", "bar");
    h.kv.emplace_back("Authorization", "Basic ZGVhZDpiZWVm");
    auto r = runResult<HttpResponse>([&] { return cli.get("/", h); });
    ASSERT_TRUE(r.has_value()) << r.error().message;
    const auto seen = srv.lastRequest();
    const auto lower = toLower(seen);
    EXPECT_NE(lower.find("x-foo: bar"), std::string::npos) << "X-Foo missing in: [" << seen << "]";
    EXPECT_NE(lower.find("authorization: basic zgvhzdpizwvm"), std::string::npos)
        << "Authorization missing in: [" << seen << "]";
}

TEST_F(HttpClientTest, RetryThenSuccess) {
    FakeServer srv{};
    srv.queueBehavior(FakeServer::Behavior::CloseImmediately); // attempt 1 fails
    // attempts 2..N use the default Reply behavior

    UpstreamConfig cfg;
    cfg.networkRetries = 3;
    cfg.readTimeout = std::chrono::seconds{2};
    HttpClient cli{baseUrl(srv.port()), cfg, lt_.loop()};

    auto r = runResult<HttpResponse>([&] { return cli.get("/retryable", Headers{}); });
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->status, 200);
    EXPECT_GE(srv.connectionCount(), 2);
}

TEST_F(HttpClientTest, RetryExhaustionReturnsUpstreamUnavailable) {
    FakeServer srv{FakeServer::defaultOk(), FakeServer::Behavior::CloseImmediately};
    UpstreamConfig cfg;
    cfg.networkRetries = 2;
    cfg.readTimeout = std::chrono::seconds{1};
    HttpClient cli{baseUrl(srv.port()), cfg, lt_.loop()};

    auto r = runResult<HttpResponse>([&] { return cli.get("/down", Headers{}); });
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ErrorCode::UpstreamUnavailable);
    EXPECT_EQ(srv.connectionCount(), 2);
}

#ifndef AID_SANITIZE
// Sanitizers slow the network path enough that the readTimeout race becomes
// flaky. Gate behind AID_SANITIZE the same way test_wal.cpp does for its
// concurrency-sensitive case.
TEST_F(HttpClientTest, TimeoutMapsToUpstreamTimeout) {
    FakeServer srv{FakeServer::defaultOk(), FakeServer::Behavior::AcceptThenHang};
    UpstreamConfig cfg;
    cfg.networkRetries = 1;
    cfg.readTimeout = std::chrono::seconds{1};
    HttpClient cli{baseUrl(srv.port()), cfg, lt_.loop()};

    auto r = runResult<HttpResponse>([&] { return cli.get("/slow", Headers{}); });
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ErrorCode::UpstreamTimeout);
}
#endif

TEST_F(HttpClientTest, ConnectionRefusedReturnsUpstreamUnavailable) {
    // Port 1 is a privileged port with effectively zero chance of a listener
    // in a normal user-mode test environment.
    UpstreamConfig cfg;
    cfg.networkRetries = 1;
    cfg.readTimeout = std::chrono::seconds{1};
    HttpClient cli{"http://127.0.0.1:1", cfg, lt_.loop()};

    auto r = runResult<HttpResponse>([&] { return cli.get("/", Headers{}); });
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ErrorCode::UpstreamUnavailable);
}

// Two requests, back-to-back, on the same HttpClient. The follow-on request
// (here a POST) must reach the server even when the first request's connection
// was a kept-alive one the server then closed — the exact OpenProject
// GET-then-PATCH/POST shape. Free coroutine (not a coroutine-lambda) so its
// frame holds `cli` by reference with no dangling-capture hazard.
[[nodiscard]] static Task<Result<HttpResponse>> getThenPost(HttpClient& cli) {
    auto first = co_await cli.get("/first", Headers{});
    if (!first.has_value()) {
        co_return first;
    }
    co_return co_await cli.post("/second", std::string_view{"{}"}, Headers{});
}

TEST_F(HttpClientTest, FollowOnWriteAfterServerClosedKeepAliveSucceeds) {
    // Canned reply has NO `Connection: close` (drogon treats it as keep-alive),
    // and FakeServer closes the socket after each reply — mimicking Apache
    // closing an idle kept-alive connection. Pre-fix, the POST reused the
    // first request's stale pooled connection, was buffered without a
    // reconnect, and never sent (timing out / hanging). Post-fix, each request
    // builds a fresh client, so the POST opens its own connection.
    FakeServer srv{"HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello"};

    UpstreamConfig cfg;
    cfg.networkRetries = 1; // single attempt: connectionCount reflects reuse, not retries
    cfg.readTimeout = std::chrono::seconds{10};
    HttpClient cli{baseUrl(srv.port()), cfg, lt_.loop()};

    auto r = runResult<HttpResponse>([&] { return getThenPost(cli); });
    ASSERT_TRUE(r.has_value()) << "follow-on POST did not complete: " << r.error().message;
    EXPECT_EQ(r->status, 200);
    EXPECT_NE(srv.lastRequest().find("POST /second"), std::string::npos)
        << "POST never reached the server: [" << srv.lastRequest() << "]";
    // One connection per request: no reuse of the server-closed connection.
    EXPECT_EQ(srv.connectionCount(), 2);
}

TEST_F(HttpClientTest, ReportSendsMethodLineAndBody) {
    // CardDAV REPORT is enabled via cmake/patches/drogon-add-report-verb.patch.
    // Verify Drogon serialises the method as the literal string "REPORT" and
    // forwards the body byte-exact (the DaviCal adapter relies on both).
    FakeServer srv{};
    HttpClient cli{baseUrl(srv.port()), UpstreamConfig{}, lt_.loop()};
    const std::string body =
        R"(<?xml version="1.0"?><C:addressbook-query xmlns:C="urn:ietf:params:xml:ns:carddav"/>)";
    Headers h;
    h.kv.emplace_back("Depth", "1");
    h.kv.emplace_back("Content-Type", "application/xml; charset=utf-8");
    auto r = runResult<HttpResponse>(
        [&] { return cli.report("/davical/caldav.php/aid/addresses/", body, h); });
    ASSERT_TRUE(r.has_value()) << r.error().message;
    const auto seen = srv.lastRequest();
    const auto lower = toLower(seen);
    EXPECT_NE(seen.find("REPORT /davical/caldav.php/aid/addresses/"), std::string::npos)
        << "REPORT method-line missing in: [" << seen << "]";
    EXPECT_NE(lower.find("depth: 1"), std::string::npos)
        << "Depth header missing in: [" << seen << "]";
    EXPECT_NE(seen.find(body), std::string::npos) << "body missing in: [" << seen << "]";
    EXPECT_NE(lower.find("content-length: " + std::to_string(body.size())), std::string::npos)
        << "content-length missing in: [" << seen << "]";
}

// cancelInFlight() must resume a request stuck in an
// upstream co_await PROMPTLY — not wait out readTimeout. This is the unit-level
// proof of the mechanism the shutdown path relies on (driven via the port's
// cancelPendingRequests() hook during the graceful drain) to stop a mailbox
// worker from being destroyed while still suspended in HTTP.
TEST_F(HttpClientTest, CancelInFlightResumesPendingRequestPromptly) {
    FakeServer srv{FakeServer::defaultOk(), FakeServer::Behavior::AcceptThenHangLong};
    UpstreamConfig cfg;
    cfg.networkRetries = 1;
    cfg.readTimeout = std::chrono::seconds{30}; // long: only the cancel should resolve it
    HttpClient cli{baseUrl(srv.port()), cfg, lt_.loop()};

    auto fut = launch<HttpResponse>([&] { return cli.get("/hang", Headers{}); });

    // The request is provably in flight once the server has read it.
    ASSERT_TRUE(waitFor([&] { return srv.connectionCount() >= 1 && !srv.lastRequest().empty(); }))
        << "request never reached the hanging server";

    const auto start = std::chrono::steady_clock::now();
    const std::size_t signalled = cli.cancelInFlight();
    EXPECT_EQ(signalled, 1u) << "exactly one request should have been outstanding";

    ASSERT_EQ(fut.wait_for(std::chrono::seconds{5}), std::future_status::ready)
        << "cancel must resolve the request well before the 30 s readTimeout";
    const auto r = fut.get();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_LT(elapsed, std::chrono::seconds{5});

    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ErrorCode::UpstreamUnavailable);
    EXPECT_NE(r.error().message.find("cancelled"), std::string::npos)
        << "cancelled request should carry a 'cancelled' message, got: " << r.error().message;
}

// With nothing outstanding, cancelInFlight() signals zero requests. It is a
// shutdown-only operation, so it is also TERMINAL: it flips the wrapper into a
// cancelled state and any subsequent send() short-circuits with a cancelled
// error rather than racing a new upstream call onto a dying client. (In
// production it is called once via the port's cancelPendingRequests() hook
// during shutdown, after which the plugin is released — so no further sends
// ever occur.)
TEST_F(HttpClientTest, CancelInFlightIsTerminalForTheWrapper) {
    FakeServer srv{};
    HttpClient cli{baseUrl(srv.port()), UpstreamConfig{}, lt_.loop()};
    EXPECT_EQ(cli.cancelInFlight(), 0u) << "no requests were outstanding";

    const auto r = runResult<HttpResponse>([&] { return cli.get("/x", Headers{}); });
    ASSERT_FALSE(r.has_value()) << "a send after cancelInFlight() must short-circuit";
    EXPECT_EQ(r.error().code, ErrorCode::UpstreamUnavailable);
    EXPECT_NE(r.error().message.find("cancelled"), std::string::npos);
}

} // namespace
