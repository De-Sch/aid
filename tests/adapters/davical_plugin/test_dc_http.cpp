// Tests for DcHttp — the thin CardDAV REPORT wrapper around HttpClient.
//
// Driving the real HttpClient against a tiny TCP server fake mirrors
// tests/infrastructure/test_http_client.cpp: it exercises the full
// wire path (Basic auth + Depth + Content-Type + REPORT method) and
// the response-status → Error mapping without standing up Drogon's
// production stack.

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <trantor/net/EventLoop.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstdint>
#include <cstring>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

#include "aid/adapters/davical/internal/DcHttp.h"
#include "aid/infrastructure/HttpClient.h"
#include "aid/plumbing/Error.h"
#include "aid/plumbing/Result.h"
#include "aid/plumbing/Task.h"

namespace aid::adapters::davical::internal::test {

namespace {

// EventLoop on its own thread — same pattern as test_http_client.cpp
// and test_plugin_smoke.cpp.
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

// Minimal HTTP/1.1 server with configurable response. Captures the
// single request bytes for assertion in the test. Closes after one
// reply per connection (Connection: close).
class FakeReportServer {
public:
    explicit FakeReportServer(std::string canned)
        : canned_(std::move(canned)), closeBeforeReply_(false) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) {
            ADD_FAILURE() << "socket: " << std::strerror(errno);
            return;
        }
        const int yes = 1;
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

    FakeReportServer(const FakeReportServer&) = delete;
    FakeReportServer& operator=(const FakeReportServer&) = delete;
    FakeReportServer(FakeReportServer&&) = delete;
    FakeReportServer& operator=(FakeReportServer&&) = delete;

    ~FakeReportServer() {
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

    [[nodiscard]] std::string lastRequest() const {
        std::lock_guard lk{mtx_};
        return lastRequest_;
    }

    void setCloseBeforeReply(bool v) noexcept {
        closeBeforeReply_.store(v, std::memory_order_release);
    }

    // Override the canned reply for a specific request method (e.g. answer
    // GET with 405 but PROPFIND with 207, mirroring a live CardDAV
    // collection). Methods without an override fall back to the ctor canned.
    void setMethodResponse(std::string method, std::string canned) {
        std::lock_guard lk{mtx_};
        methodResponses_[std::move(method)] = std::move(canned);
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
                return;
            }
            if (closeBeforeReply_.load(std::memory_order_acquire)) {
                ::close(client);
                continue;
            }
            std::string req;
            char buf[2048];
            while (req.find("\r\n\r\n") == std::string::npos) {
                const ssize_t n = ::read(client, buf, sizeof(buf));
                if (n <= 0) {
                    ::close(client);
                    goto next;
                }
                req.append(buf, static_cast<std::size_t>(n));
            }
            // Find Content-Length, read body
            {
                std::string lower;
                lower.reserve(req.size());
                for (const char c : req) {
                    lower.push_back(static_cast<char>(::tolower(static_cast<unsigned char>(c))));
                }
                const auto clPos = lower.find("content-length:");
                const auto headerEnd = req.find("\r\n\r\n");
                if (clPos != std::string::npos && headerEnd != std::string::npos) {
                    const auto valStart = clPos + std::string{"content-length:"}.size();
                    const auto eol = lower.find("\r\n", valStart);
                    const auto valStr =
                        lower.substr(valStart, eol == std::string::npos ? eol : eol - valStart);
                    const auto contentLength = static_cast<std::size_t>(std::stoul(valStr));
                    const auto bodyStart = headerEnd + 4;
                    while (req.size() < bodyStart + contentLength) {
                        const ssize_t n = ::read(client, buf, sizeof(buf));
                        if (n <= 0) {
                            break;
                        }
                        req.append(buf, static_cast<std::size_t>(n));
                    }
                }
            }
            {
                std::string method;
                const auto sp = req.find(' ');
                if (sp != std::string::npos) {
                    method = req.substr(0, sp);
                }
                std::string reply;
                {
                    std::lock_guard lk{mtx_};
                    lastRequest_ = req;
                    const auto it = methodResponses_.find(method);
                    reply = (it != methodResponses_.end()) ? it->second : canned_;
                }
                (void)::write(client, reply.data(), reply.size());
            }
            ::close(client);
        next:;
        }
    }

    std::string canned_;
    std::atomic<bool> closeBeforeReply_;
    std::thread thread_;
    int fd_{-1};
    std::uint16_t port_{0};
    std::atomic<bool> stop_{false};
    mutable std::mutex mtx_;
    std::string lastRequest_;
    std::unordered_map<std::string, std::string> methodResponses_;
};

[[nodiscard]] std::string toLower(std::string s) {
    for (auto& c : s) {
        c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

// Runs a coroutine factory on the loop thread (HttpClient is pinned
// there) and blocks for the result. Same shape as
// HttpClientTest::runResult — held in an anonymous namespace because
// it's specific to this test fixture.
class DcHttpTest : public ::testing::Test {
protected:
    void TearDown() override {
        std::lock_guard lk{inFlightMtx_};
        inFlight_.clear();
    }

    template <class T>
    aid::plumbing::Result<T>
    runResult(std::function<aid::plumbing::Task<aid::plumbing::Result<T>>()> factory) {
        auto p = std::make_shared<std::promise<aid::plumbing::Result<T>>>();
        auto fut = p->get_future();
        const int slot = nextSlot_.fetch_add(1, std::memory_order_relaxed);

        lt_.loop().queueInLoop([this, factory = std::move(factory), p, slot]() mutable {
            auto coro = [](std::function<aid::plumbing::Task<aid::plumbing::Result<T>>()> f,
                           std::shared_ptr<std::promise<aid::plumbing::Result<T>>> prom, int slotId,
                           DcHttpTest* self) -> aid::plumbing::Task<void> {
                try {
                    auto r = co_await f();
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
            inFlight_.emplace(slot, std::make_unique<aid::plumbing::Task<void>>(std::move(coro)));
        });
        return fut.get();
    }

    [[nodiscard]] std::string baseUrl(std::uint16_t port) const {
        return "http://127.0.0.1:" + std::to_string(port);
    }

    LoopThread lt_{};
    std::mutex inFlightMtx_;
    std::unordered_map<int, std::unique_ptr<aid::plumbing::Task<void>>> inFlight_;
    std::atomic<int> nextSlot_{0};
};

} // namespace

TEST_F(DcHttpTest, ReportSendsBasicAuthDepthAndContentTypeHeaders) {
    FakeReportServer srv{FakeReportServer::with(207, "Multi-Status", "<d:multistatus/>")};
    aid::infrastructure::HttpClient cli{baseUrl(srv.port()), aid::infrastructure::UpstreamConfig{},
                                        lt_.loop()};
    DcHttp dc{cli, "aid", "1234"};
    const std::string xml = "<?xml version=\"1.0\"?><C:addressbook-query/>";
    auto r = runResult<std::string>([&] { return dc.report("/davical/aid/", xml); });
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(*r, "<d:multistatus/>");

    const auto seen = srv.lastRequest();
    const auto lower = toLower(seen);
    EXPECT_NE(seen.find("REPORT /davical/aid/"), std::string::npos)
        << "REPORT method-line missing in: [" << seen << "]";
    EXPECT_NE(lower.find("depth: 1"), std::string::npos)
        << "Depth header missing in: [" << seen << "]";
    EXPECT_NE(lower.find("content-type: application/xml"), std::string::npos)
        << "Content-Type header missing in: [" << seen << "]";
    // base64("aid:1234") = "YWlkOjEyMzQ="
    EXPECT_NE(lower.find("authorization: basic ywlkojeymzq="), std::string::npos)
        << "Basic auth missing or wrong in: [" << seen << "]";
    EXPECT_NE(seen.find(xml), std::string::npos) << "body missing in: [" << seen << "]";
}

TEST_F(DcHttpTest, Report401MapsToUnauthenticated) {
    FakeReportServer srv{FakeReportServer::with(401, "Unauthorized", "auth failed")};
    aid::infrastructure::HttpClient cli{baseUrl(srv.port()), aid::infrastructure::UpstreamConfig{},
                                        lt_.loop()};
    DcHttp dc{cli, "aid", "wrong"};
    auto r = runResult<std::string>([&] { return dc.report("/davical/aid/", "<x/>"); });
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, aid::plumbing::ErrorCode::Unauthenticated);
    EXPECT_NE(r.error().message.find("401"), std::string::npos);
}

TEST_F(DcHttpTest, Report500MapsToUpstreamUnavailable) {
    FakeReportServer srv{FakeReportServer::with(500, "Internal Server Error", "oops")};
    aid::infrastructure::HttpClient cli{baseUrl(srv.port()), aid::infrastructure::UpstreamConfig{},
                                        lt_.loop()};
    DcHttp dc{cli, "aid", "1234"};
    auto r = runResult<std::string>([&] { return dc.report("/davical/aid/", "<x/>"); });
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, aid::plumbing::ErrorCode::UpstreamUnavailable);
}

TEST_F(DcHttpTest, Report404MapsToNotFound) {
    FakeReportServer srv{FakeReportServer::with(404, "Not Found", "missing")};
    aid::infrastructure::HttpClient cli{baseUrl(srv.port()), aid::infrastructure::UpstreamConfig{},
                                        lt_.loop()};
    DcHttp dc{cli, "aid", "1234"};
    auto r = runResult<std::string>([&] { return dc.report("/davical/aid/", "<x/>"); });
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, aid::plumbing::ErrorCode::NotFound);
}

TEST_F(DcHttpTest, ReportNetworkErrorPropagates) {
    // Server closes the connection immediately (no reply); HttpClient
    // sees NetworkFailure → UpstreamUnavailable, and DcHttp surfaces
    // that unchanged.
    FakeReportServer srv{FakeReportServer::with(200, "OK", "")};
    srv.setCloseBeforeReply(true);
    aid::infrastructure::UpstreamConfig cfg;
    cfg.networkRetries = 1;
    cfg.readTimeout = std::chrono::seconds{1};
    aid::infrastructure::HttpClient cli{baseUrl(srv.port()), cfg, lt_.loop()};
    DcHttp dc{cli, "aid", "1234"};
    auto r = runResult<std::string>([&] { return dc.report("/davical/aid/", "<x/>"); });
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, aid::plumbing::ErrorCode::UpstreamUnavailable);
}

TEST_F(DcHttpTest, ReportLargeErrorBodyIsTruncated) {
    // Cap body in Error::message at 200 chars so we
    // don't dump arbitrary upstream bytes into the daemon's logs.
    const std::string longBody(500, 'x');
    FakeReportServer srv{FakeReportServer::with(500, "Internal Server Error", longBody)};
    aid::infrastructure::HttpClient cli{baseUrl(srv.port()), aid::infrastructure::UpstreamConfig{},
                                        lt_.loop()};
    DcHttp dc{cli, "aid", "1234"};
    auto r = runResult<std::string>([&] { return dc.report("/davical/aid/", "<x/>"); });
    ASSERT_FALSE(r.has_value());
    EXPECT_LT(r.error().message.size(), longBody.size())
        << "error message should be truncated, got len=" << r.error().message.size();
}

// --- probe(): DaviCal reachability ------------------------------------------
// A CardDAV collection answers GET with 405; the probe must use PROPFIND so a
// healthy book (207 Multi-Status) reads as reachable on /health.

TEST_F(DcHttpTest, ProbeUsesPropfindDepth0) {
    FakeReportServer srv{FakeReportServer::with(207, "Multi-Status", "<d:multistatus/>")};
    aid::infrastructure::HttpClient cli{baseUrl(srv.port()), aid::infrastructure::UpstreamConfig{},
                                        lt_.loop()};
    DcHttp dc{cli, "aid", "1234"};
    auto r = runResult<void>([&] { return dc.probe("/davical/aid/addresses/"); });
    ASSERT_TRUE(r.has_value()) << r.error().message;

    const auto seen = srv.lastRequest();
    const auto lower = toLower(seen);
    EXPECT_NE(seen.find("PROPFIND /davical/aid/addresses/"), std::string::npos)
        << "PROPFIND method-line missing in: [" << seen << "]";
    EXPECT_NE(lower.find("depth: 0"), std::string::npos)
        << "Depth: 0 header missing in: [" << seen << "]";
    EXPECT_EQ(seen.find("GET "), std::string::npos)
        << "probe must not use GET (a collection answers GET with 405): [" << seen << "]";
}

TEST_F(DcHttpTest, Probe405OnGetButReachableViaPropfind) {
    // Mirrors live DaviCal exactly: GET on the collection -> 405, PROPFIND ->
    // 207. The fix means probe() issues PROPFIND, so the book reads reachable
    // and /health no longer reports davical:"unreachable".
    FakeReportServer srv{
        FakeReportServer::with(405, "Method Not Allowed", "no GET on a collection")};
    srv.setMethodResponse("PROPFIND",
                          FakeReportServer::with(207, "Multi-Status", "<d:multistatus/>"));
    aid::infrastructure::HttpClient cli{baseUrl(srv.port()), aid::infrastructure::UpstreamConfig{},
                                        lt_.loop()};
    DcHttp dc{cli, "aid", "1234"};
    auto r = runResult<void>([&] { return dc.probe("/davical/aid/addresses/"); });
    EXPECT_TRUE(r.has_value()) << "PROPFIND 207 must read as reachable, got error: "
                               << (r.has_value() ? std::string{} : r.error().message);
}

TEST_F(DcHttpTest, Probe405MarksUnreachable) {
    // A genuinely broken book (405 even to PROPFIND) must still surface as an
    // error so /health doesn't go falsely green.
    FakeReportServer srv{FakeReportServer::with(405, "Method Not Allowed", "nope")};
    aid::infrastructure::HttpClient cli{baseUrl(srv.port()), aid::infrastructure::UpstreamConfig{},
                                        lt_.loop()};
    DcHttp dc{cli, "aid", "1234"};
    auto r = runResult<void>([&] { return dc.probe("/davical/aid/addresses/"); });
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, aid::plumbing::ErrorCode::UpstreamUnavailable);
}

} // namespace aid::adapters::davical::internal::test
