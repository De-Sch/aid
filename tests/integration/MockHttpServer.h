#pragma once

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// A minimal raw-socket HTTP/1.1 server for integration tests that need a real
// network endpoint behind the daemon's HttpClient (e.g. driving the real
// OpenProject / DaviCal plugin .so against canned upstream responses). It
// generalizes the path-routed server in tests/adapters/*/test_plugin_smoke.cpp
// and the FakeReportServer in tests/adapters/davical_plugin/test_dc_http.cpp:
//   * routes on (method, target) via a caller-supplied responder lambda, so a
//     single server answers GET/POST/PATCH (OpenProject) or REPORT/PROPFIND
//     (DaviCal CardDAV) with status + content-type + body the test chooses;
//   * records every request (method, target, body) for assertions.
// One accept thread, one reply per connection (Connection: close). Binds
// 127.0.0.1:0 so the OS assigns a free port (read back via port()).

namespace aid::tests::integration {

struct MockRequest {
    std::string method;
    std::string target; // request-target: path + optional ?query
    std::string body;
};

struct MockResponse {
    int status{200};
    std::string statusText{"OK"};
    std::string contentType{"application/json"};
    std::string body;
};

using MockResponder = std::function<MockResponse(const MockRequest&)>;

class MockHttpServer {
public:
    explicit MockHttpServer(MockResponder responder) : responder_(std::move(responder)) {
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
        if (::listen(fd_, 16) != 0) {
            ADD_FAILURE() << "listen: " << std::strerror(errno);
            return;
        }
        thread_ = std::thread([this] { serve(); });
    }

    MockHttpServer(const MockHttpServer&) = delete;
    MockHttpServer& operator=(const MockHttpServer&) = delete;
    MockHttpServer(MockHttpServer&&) = delete;
    MockHttpServer& operator=(MockHttpServer&&) = delete;

    ~MockHttpServer() {
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

    // Connections fully handled (responder returned, reply written, socket
    // closed). Lets a test know the server side of an in-flight request is done
    // — e.g. after cancelling it — so the client loop can reclaim the connection
    // before teardown rather than leaking it.
    [[nodiscard]] int connectionsCompleted() const noexcept {
        return completed_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::vector<MockRequest> requests() const {
        std::lock_guard lk{mtx_};
        return requests_;
    }

    // Count recorded requests whose (method, target) match a predicate.
    template <class Pred> [[nodiscard]] std::size_t count(Pred&& pred) const {
        std::lock_guard lk{mtx_};
        std::size_t n = 0;
        for (const auto& r : requests_) {
            if (pred(r)) {
                ++n;
            }
        }
        return n;
    }

private:
    void serve() {
        while (!stop_.load(std::memory_order_acquire)) {
            const int client = ::accept(fd_, nullptr, nullptr);
            if (client < 0) {
                return; // listen socket shut down
            }
            handle(client);
            ::close(client);
            completed_.fetch_add(1, std::memory_order_release);
        }
    }

    void handle(int client) {
        std::string req;
        char buf[2048];
        while (req.find("\r\n\r\n") == std::string::npos) {
            const ssize_t n = ::read(client, buf, sizeof(buf));
            if (n <= 0) {
                return;
            }
            req.append(buf, static_cast<std::size_t>(n));
        }

        const auto headerEnd = req.find("\r\n\r\n");
        std::string lower;
        lower.reserve(headerEnd);
        for (std::size_t i = 0; i < headerEnd; ++i) {
            lower.push_back(static_cast<char>(::tolower(static_cast<unsigned char>(req[i]))));
        }
        if (const auto clPos = lower.find("content-length:"); clPos != std::string::npos) {
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

        MockRequest parsed = parseRequest(req);
        MockResponse resp = responder_(parsed);
        {
            std::lock_guard lk{mtx_};
            requests_.push_back(std::move(parsed));
        }

        const std::string wire = "HTTP/1.1 " + std::to_string(resp.status) + " " + resp.statusText +
                                 "\r\nContent-Type: " + resp.contentType +
                                 "\r\nContent-Length: " + std::to_string(resp.body.size()) +
                                 "\r\nConnection: close\r\n\r\n" + resp.body;
        (void)::write(client, wire.data(), wire.size());
    }

    static MockRequest parseRequest(const std::string& req) {
        MockRequest out;
        const auto sp1 = req.find(' ');
        if (sp1 != std::string::npos) {
            out.method = req.substr(0, sp1);
            const auto sp2 = req.find(' ', sp1 + 1);
            if (sp2 != std::string::npos) {
                out.target = req.substr(sp1 + 1, sp2 - sp1 - 1);
            }
        }
        if (const auto bodyStart = req.find("\r\n\r\n"); bodyStart != std::string::npos) {
            out.body = req.substr(bodyStart + 4);
        }
        return out;
    }

    MockResponder responder_;
    std::thread thread_;
    int fd_{-1};
    std::uint16_t port_{0};
    std::atomic<bool> stop_{false};
    std::atomic<int> completed_{0};
    mutable std::mutex mtx_;
    std::vector<MockRequest> requests_;
};

} // namespace aid::tests::integration
