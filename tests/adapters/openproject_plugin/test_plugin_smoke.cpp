// Plugin .so smoke test — dlopen the real aid_openproject_plugin.so via
// the new loop-aware PluginLoader overload, hand it config_json + a
// trantor::EventLoop*, and verify one call (resolveUser) round-trips
// through a tiny local HTTP server. Acts as the integration check that
// the extern "C" factory triplet, config parsing, HttpClient wiring,
// and TicketStore interface match across the .so boundary.
//
// The test deliberately avoids exercising every code path — those live
// in the per-class unit tests against FakeHttpDispatcher. This file's
// job is to catch ABI / link-order / symbol-visibility regressions.

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
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>

#include "aid/infrastructure/PluginLoader.h"
#include "aid/ports/TicketStore.h"

#ifndef AID_OPENPROJECT_PLUGIN_PATH
#error "AID_OPENPROJECT_PLUGIN_PATH must be defined by CMake"
#endif

namespace {

// Path → canned-response server. Same pattern as the HttpClient test,
// but routed by request path so a single server can answer the
// resolveUser query and any preflight queries the adapter issues.
class PathRouterServer {
public:
    explicit PathRouterServer(std::unordered_map<std::string, std::string> responses)
        : responses_(std::move(responses)) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        ASSERT_TRUE_RES(fd_ >= 0, "socket failed");
        const int yes = 1;
        ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        ::sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        ASSERT_TRUE_RES(::bind(fd_, reinterpret_cast<::sockaddr*>(&addr), sizeof(addr)) == 0,
                        "bind failed");
        ::socklen_t len = sizeof(addr);
        ASSERT_TRUE_RES(::getsockname(fd_, reinterpret_cast<::sockaddr*>(&addr), &len) == 0,
                        "getsockname failed");
        port_ = ntohs(addr.sin_port);

        ASSERT_TRUE_RES(::listen(fd_, 8) == 0, "listen failed");
        thread_ = std::thread([this] { serve(); });
    }

    ~PathRouterServer() {
        stop_.store(true, std::memory_order_release);
        if (fd_ >= 0) {
            ::shutdown(fd_, SHUT_RDWR);
            ::close(fd_);
            fd_ = -1;
        }
        if (thread_.joinable())
            thread_.join();
    }

    PathRouterServer(const PathRouterServer&) = delete;
    PathRouterServer& operator=(const PathRouterServer&) = delete;
    PathRouterServer(PathRouterServer&&) = delete;
    PathRouterServer& operator=(PathRouterServer&&) = delete;

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

    [[nodiscard]] int requestCount() const noexcept {
        return requestCount_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::string lastPath() const {
        std::lock_guard lk{mtx_};
        return lastPath_;
    }

private:
    static void ASSERT_TRUE_RES(bool cond, const char* msg) {
        if (!cond) {
            ADD_FAILURE() << msg << ": " << std::strerror(errno);
        }
    }

    static std::string buildResponse(int status, std::string_view body) {
        std::string out = "HTTP/1.1 " + std::to_string(status) +
                          (status == 200 ? " OK" : " Other") +
                          "\r\nContent-Type: application/json"
                          "\r\nContent-Length: " +
                          std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
        out.append(body);
        return out;
    }

    void serve() {
        while (!stop_.load(std::memory_order_acquire)) {
            const int client = ::accept(fd_, nullptr, nullptr);
            if (client < 0)
                return;
            handle(client);
            ::close(client);
        }
    }

    void handle(int client) {
        std::string req;
        char buf[2048];
        while (req.find("\r\n\r\n") == std::string::npos) {
            const ssize_t n = ::read(client, buf, sizeof(buf));
            if (n <= 0)
                return;
            req.append(buf, static_cast<std::size_t>(n));
        }
        // Extract path from "GET /foo HTTP/1.1\r\n…"
        std::string path;
        if (const auto sp1 = req.find(' '); sp1 != std::string::npos) {
            if (const auto sp2 = req.find(' ', sp1 + 1); sp2 != std::string::npos) {
                path = req.substr(sp1 + 1, sp2 - sp1 - 1);
            }
        }
        requestCount_.fetch_add(1, std::memory_order_acq_rel);
        {
            std::lock_guard lk{mtx_};
            lastPath_ = path;
        }

        // Path prefix match — the URL contains a complex `?filters=…`
        // query, so test setup keys the response on the prefix only.
        std::string responseBody;
        bool matched = false;
        for (const auto& [prefix, body] : responses_) {
            if (path.size() >= prefix.size() && path.compare(0, prefix.size(), prefix) == 0) {
                responseBody = body;
                matched = true;
                break;
            }
        }
        if (!matched)
            responseBody = R"({"_embedded":{"elements":[]}})";

        const std::string resp = buildResponse(200, responseBody);
        const ssize_t w = ::write(client, resp.data(), resp.size());
        (void)w;
    }

    std::unordered_map<std::string, std::string> responses_;
    std::thread thread_;
    int fd_{-1};
    std::uint16_t port_{0};
    std::atomic<bool> stop_{false};
    std::atomic<int> requestCount_{0};
    mutable std::mutex mtx_;
    std::string lastPath_;
};

// LoopThread — own EventLoop on a dedicated thread (same pattern as
// HttpClient tests).
class LoopThread {
public:
    LoopThread() {
        std::promise<trantor::EventLoop*> ready;
        auto fut = ready.get_future();
        thread_ = std::thread([&ready] {
            trantor::EventLoop loop;
            ready.set_value(&loop);
            loop.loop();
        });
        loop_ = fut.get();
    }
    ~LoopThread() {
        if (loop_ != nullptr) {
            loop_->queueInLoop([loop = loop_] { loop->quit(); });
        }
        if (thread_.joinable())
            thread_.join();
    }
    LoopThread(const LoopThread&) = delete;
    LoopThread& operator=(const LoopThread&) = delete;
    LoopThread(LoopThread&&) = delete;
    LoopThread& operator=(LoopThread&&) = delete;
    [[nodiscard]] trantor::EventLoop& loop() const noexcept { return *loop_; }

private:
    std::thread thread_;
    trantor::EventLoop* loop_{nullptr};
};

std::string buildConfigJson(std::uint16_t port) {
    return std::string{R"({
        "baseUrl": "http://127.0.0.1:)"} +
           std::to_string(port) + std::string{R"(",
        "apiToken": "smoke-token",
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

} // namespace

TEST(OpenProjectPluginSmoke, PluginLoaderLoadsWithLoopAndApiVersionMatches) {
    PathRouterServer server({});
    LoopThread lt;
    const std::string cfg = buildConfigJson(server.port());

    aid::infrastructure::PluginLoader<aid::ports::TicketStore> loader;
    auto r = loader.loadWithLoop(AID_OPENPROJECT_PLUGIN_PATH, "create_TicketStore",
                                 "destroy_TicketStore", cfg, &lt.loop(), ::geteuid());
    ASSERT_TRUE(r.has_value()) << r.error().message;
    ASSERT_NE(loader.get(), nullptr);

    auto v = loader.apiVersion();
    ASSERT_TRUE(v.has_value());
    ASSERT_TRUE(v->has_value());
    EXPECT_EQ(**v, 1);
}

TEST(OpenProjectPluginSmoke, ResolveUserRoundTripsThroughDlopenedPlugin) {
    // Fake OpenProject: /api/v3/users?filters=… returns a single user
    // with login "alice".
    std::unordered_map<std::string, std::string> responses;
    responses["/api/v3/users"] = R"({
        "_embedded": {
            "elements": [
                {"login":"alice", "id":9, "_links":{"self":{"href":"/api/v3/users/9"}}}
            ]
        }
    })";
    PathRouterServer server(std::move(responses));

    // `inflight` holds the driver coroutine's own Task while it runs; the
    // coroutine schedules `inflight.tasks.erase(...)` back on the loop after it
    // completes. It MUST outlive the loop thread — declare it BEFORE `lt` so it
    // is destroyed only AFTER `~LoopThread` quits and joins the loop. Otherwise
    // the loop thread can run that erase while this stack frame is being torn
    // down, racing the map's destruction (heap corruption — a real intermittent
    // SIGSEGV before this ordering fix).
    struct Inflight {
        std::mutex mtx;
        std::unordered_map<int, std::unique_ptr<aid::plumbing::Task<void>>> tasks;
    };
    Inflight inflight;

    LoopThread lt;
    const std::string cfg = buildConfigJson(server.port());

    aid::infrastructure::PluginLoader<aid::ports::TicketStore> loader;
    auto r = loader.loadWithLoop(AID_OPENPROJECT_PLUGIN_PATH, "create_TicketStore",
                                 "destroy_TicketStore", cfg, &lt.loop(), ::geteuid());
    ASSERT_TRUE(r.has_value()) << r.error().message;
    aid::ports::TicketStore* store = loader.get();
    ASSERT_NE(store, nullptr);

    // Drive resolveUser on the loop thread (HttpClient is pinned to that
    // loop, awaiter chain resumes on the same thread). The driver
    // coroutine holds its own Task<void> in a heap slot; after the
    // promise is set, it schedules a cleanup of that slot back on the
    // loop. Same pattern as tests/infrastructure/test_http_client.cpp.
    std::promise<aid::plumbing::Result<std::optional<aid::UserHandle>>> p;
    auto fut = p.get_future();

    lt.loop().queueInLoop([store, &p, &inflight, &lt] {
        const int slot = 1;
        auto coro = [](aid::ports::TicketStore* s,
                       std::promise<aid::plumbing::Result<std::optional<aid::UserHandle>>>& prom,
                       Inflight* inf, trantor::EventLoop* loop,
                       int slotId) -> aid::plumbing::Task<void> {
            try {
                auto resolved = co_await s->resolveUser("alice");
                prom.set_value(std::move(resolved));
            } catch (...) {
                prom.set_exception(std::current_exception());
            }
            loop->queueInLoop([inf, slotId] {
                std::lock_guard lk{inf->mtx};
                inf->tasks.erase(slotId);
            });
        }(store, p, &inflight, &lt.loop(), slot);
        std::lock_guard lk{inflight.mtx};
        inflight.tasks.emplace(slot, std::make_unique<aid::plumbing::Task<void>>(std::move(coro)));
    });

    const auto status = fut.wait_for(std::chrono::seconds{5});
    ASSERT_EQ(status, std::future_status::ready) << "resolveUser timed out";

    auto resolved = fut.get();
    ASSERT_TRUE(resolved.has_value()) << resolved.error().message;
    ASSERT_TRUE(resolved->has_value());
    EXPECT_EQ((*resolved)->v, "alice");
    EXPECT_GE(server.requestCount(), 1);
}

TEST(OpenProjectPluginSmoke, FactoryWithMalformedConfigJsonReturnsNullptr) {
    // PluginLoader treats a nullptr factory result as PluginAbiMismatch.
    // Verifies the factory's catch-all error path actually engages.
    LoopThread lt;
    const std::string badCfg = R"({"baseUrl":"http://127.0.0.1"})"; // missing apiToken, etc.

    aid::infrastructure::PluginLoader<aid::ports::TicketStore> loader;
    auto r = loader.loadWithLoop(AID_OPENPROJECT_PLUGIN_PATH, "create_TicketStore",
                                 "destroy_TicketStore", badCfg, &lt.loop(), ::geteuid());
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, aid::plumbing::ErrorCode::PluginAbiMismatch);
    EXPECT_NE(r.error().message.find("returned nullptr"), std::string::npos);
}

TEST(OpenProjectPluginSmoke, FactoryTripletIsResolvableViaDlopen) {
    // Open the .so directly (independent of PluginLoader) and confirm
    // the three required symbols resolve. Hidden visibility of internal
    // symbols is enforced at build time (CXX_VISIBILITY_PRESET hidden +
    // AID_PLUGIN_EXPORT) and verified out-of-band by `nm -D` — checking
    // it here would race other tests in the same process.
    void* handle = ::dlopen(AID_OPENPROJECT_PLUGIN_PATH, RTLD_NOW);
    ASSERT_NE(handle, nullptr) << ::dlerror();

    EXPECT_NE(::dlsym(handle, "create_TicketStore"), nullptr);
    EXPECT_NE(::dlsym(handle, "destroy_TicketStore"), nullptr);
    EXPECT_NE(::dlsym(handle, "aid_plugin_api_version"), nullptr);
    // BF3 stale-plugin guard symbol — every plugin built after the guard
    // exports it; the daemon refuses to start without it.
    EXPECT_NE(::dlsym(handle, "aid_plugin_contract_tag"), nullptr);

    // Intentionally do NOT dlclose.
}
