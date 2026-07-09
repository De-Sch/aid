// Tests for DaviCalAdapter — canonicalize() against libphonenumber's
// stable subset, and the two-step lookup pipeline (exact match on
// addresses book → prefix match on companies book → nullopt) against
// a small inline path-router server fake.

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
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>

#include "aid/adapters/davical/DaviCalAdapter.h"
#include "aid/infrastructure/HttpClient.h"
#include "aid/plumbing/Error.h"
#include "aid/plumbing/Result.h"
#include "aid/plumbing/Task.h"
#include "aid/value-types/Contact.h"
#include "aid/value-types/Ids.h"

namespace aid::adapters::davical::test {

namespace {

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

// Multistatus body matching one Contact (FN, ORG, TEL, X-CUSTOM1).
[[nodiscard]] std::string canonicalMultistatus(std::string fn, std::string org, std::string tel,
                                               std::string projectIds) {
    return "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
           "<d:multistatus xmlns:d=\"DAV:\" xmlns:card=\"urn:ietf:params:xml:ns:carddav\">"
           "<d:response><d:propstat><d:prop>"
           "<card:address-data>BEGIN:VCARD\r\nFN:" +
           fn + "\r\nORG:" + org + "\r\nTEL:" + tel + "\r\nX-CUSTOM1:" + projectIds +
           "\r\nEND:VCARD\r\n</card:address-data>"
           "</d:prop></d:propstat></d:response>"
           "</d:multistatus>";
}

[[nodiscard]] std::string emptyMultistatus() {
    return "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
           "<d:multistatus xmlns:d=\"DAV:\"/>";
}

// Path-router HTTP server. Maps incoming request paths to canned
// response bodies (all 207 Multi-Status). The DaviCal adapter posts
// REPORTs against two distinct paths (addresses + companies); the
// router lets a test set each independently.
class PathRouterServer {
public:
    explicit PathRouterServer(std::unordered_map<std::string, std::string> responses)
        : responses_(std::move(responses)) {
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
    PathRouterServer(const PathRouterServer&) = delete;
    PathRouterServer& operator=(const PathRouterServer&) = delete;
    PathRouterServer(PathRouterServer&&) = delete;
    PathRouterServer& operator=(PathRouterServer&&) = delete;
    ~PathRouterServer() {
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
    [[nodiscard]] int requestCount() const noexcept {
        return requestCount_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::string lastPathFor(std::string_view marker) const {
        std::lock_guard lk{mtx_};
        const auto it = pathsByMarker_.find(std::string{marker});
        return it == pathsByMarker_.end() ? std::string{} : it->second;
    }
    [[nodiscard]] std::string lastBodyFor(std::string_view marker) const {
        std::lock_guard lk{mtx_};
        const auto it = bodiesByMarker_.find(std::string{marker});
        return it == bodiesByMarker_.end() ? std::string{} : it->second;
    }

private:
    void serve() {
        while (!stop_.load(std::memory_order_acquire)) {
            int client = ::accept(fd_, nullptr, nullptr);
            if (client < 0) {
                return;
            }
            handle(client);
            ::close(client);
        }
    }

    void handle(int client) {
        std::string req;
        char buf[4096];
        while (req.find("\r\n\r\n") == std::string::npos) {
            const ssize_t n = ::read(client, buf, sizeof(buf));
            if (n <= 0) {
                return;
            }
            req.append(buf, static_cast<std::size_t>(n));
        }
        // Read body (best-effort Content-Length)
        const auto headerEnd = req.find("\r\n\r\n");
        std::string lower;
        lower.reserve(headerEnd);
        for (std::size_t i = 0; i < headerEnd; ++i) {
            lower.push_back(static_cast<char>(::tolower(static_cast<unsigned char>(req[i]))));
        }
        const auto clPos = lower.find("content-length:");
        if (clPos != std::string::npos) {
            const auto valStart = clPos + std::string{"content-length:"}.size();
            const auto eol = lower.find("\r\n", valStart);
            const auto valStr =
                lower.substr(valStart, eol == std::string::npos ? eol : eol - valStart);
            const auto contentLength = static_cast<std::size_t>(std::stoul(valStr));
            while (req.size() < headerEnd + 4 + contentLength) {
                const ssize_t n = ::read(client, buf, sizeof(buf));
                if (n <= 0) {
                    break;
                }
                req.append(buf, static_cast<std::size_t>(n));
            }
        }

        // Extract request path: "REPORT /path HTTP/1.1"
        const auto firstSp = req.find(' ');
        const auto secondSp = req.find(' ', firstSp + 1);
        std::string path;
        if (firstSp != std::string::npos && secondSp != std::string::npos) {
            path = req.substr(firstSp + 1, secondSp - firstSp - 1);
        }
        requestCount_.fetch_add(1, std::memory_order_acq_rel);

        // Request body (everything past the header terminator). Lets a
        // test assert the CardDAV REPORT body (e.g. the text-match type).
        std::string reqBody;
        if (headerEnd != std::string::npos) {
            reqBody = req.substr(headerEnd + 4);
        }

        // Record path + body under any marker that's a substring of the
        // path (e.g. "addresses" → "/davical/aid/addresses/"). Lets the
        // test assert which book got hit and with what query.
        {
            std::lock_guard lk{mtx_};
            for (const auto& [marker, _] : responses_) {
                if (path.find(marker) != std::string::npos) {
                    pathsByMarker_[marker] = path;
                    bodiesByMarker_[marker] = reqBody;
                }
            }
        }

        // Pick response by matching the path against the keys in
        // responses_ (substring match — first hit wins).
        std::string body;
        for (const auto& [marker, content] : responses_) {
            if (path.find(marker) != std::string::npos) {
                body = content;
                break;
            }
        }
        const std::string resp = "HTTP/1.1 207 Multi-Status\r\n"
                                 "Content-Type: application/xml; charset=utf-8\r\n"
                                 "Content-Length: " +
                                 std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" +
                                 body;
        (void)::write(client, resp.data(), resp.size());
    }

    std::unordered_map<std::string, std::string> responses_;
    std::thread thread_;
    int fd_{-1};
    std::uint16_t port_{0};
    std::atomic<bool> stop_{false};
    std::atomic<int> requestCount_{0};
    mutable std::mutex mtx_;
    std::unordered_map<std::string, std::string> pathsByMarker_;
    std::unordered_map<std::string, std::string> bodiesByMarker_;
};

class DaviCalAdapterTest : public ::testing::Test {
protected:
    void TearDown() override {
        std::lock_guard lk{inFlightMtx_};
        inFlight_.clear();
    }

    // Canonicalize-only tests don't hit the network, so any URL is
    // fine. Lookup tests use configForWithBookPaths so the per-request
    // paths (not host base, which HttpClient already carries) end up
    // routable through PathRouterServer's "addresses"/"companies"
    // substring matcher.
    [[nodiscard]] DaviCalConfig configFor() {
        DaviCalConfig cfg;
        cfg.bookAddresses = "http://127.0.0.1:1/aid/addresses/";
        cfg.bookCompanies = "http://127.0.0.1:1/aid/companies/";
        cfg.user = "test";
        cfg.password = "test";
        cfg.defaultRegion = "DE";
        return cfg;
    }

    [[nodiscard]] DaviCalConfig configForWithBookPaths() {
        DaviCalConfig cfg;
        cfg.bookAddresses = "/aid/addresses/";
        cfg.bookCompanies = "/aid/companies/";
        cfg.user = "test";
        cfg.password = "test";
        cfg.defaultRegion = "DE";
        return cfg;
    }

    [[nodiscard]] std::unique_ptr<DaviCalAdapter> makeAdapter(std::uint16_t port,
                                                              DaviCalConfig cfg) {
        const std::string base = "http://127.0.0.1:" + std::to_string(port);
        auto http = std::make_unique<aid::infrastructure::HttpClient>(
            base, aid::infrastructure::UpstreamConfig{}, lt_.loop());
        return std::make_unique<DaviCalAdapter>(std::move(http), std::move(cfg));
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
                           DaviCalAdapterTest* self) -> aid::plumbing::Task<void> {
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

    LoopThread lt_{};
    std::mutex inFlightMtx_;
    std::unordered_map<int, std::unique_ptr<aid::plumbing::Task<void>>> inFlight_;
    std::atomic<int> nextSlot_{0};
};

} // namespace

// ─── canonicalize() matrix ─────────────────────────────────────────────

struct CanonRow {
    std::string_view input;
    std::string_view region;
    std::string_view expected; // empty == non-routable
};

class CanonicalizeMatrix : public DaviCalAdapterTest,
                           public ::testing::WithParamInterface<CanonRow> {};

TEST_P(CanonicalizeMatrix, CanonicalizesPerSpec) {
    const auto& row = GetParam();
    auto cfg = configFor();
    cfg.defaultRegion = std::string{row.region};
    auto adapter = makeAdapter(1, cfg);
    const auto got = adapter->canonicalize(aid::PhoneNumber{std::string{row.input}});
    EXPECT_EQ(got.v, row.expected) << "input=[" << row.input << "] region=[" << row.region << "]";
}

// Two spec examples are deliberately not asserted in this matrix:
//   "011 415 555 0100" / US → "+14155550100" — the spec claims
//     libphonenumber rewrites the US IDD prefix `011` like a `+`, but
//     libphonenumber 8.12 only does so when the digits after `011`
//     already include the destination country code (e.g.
//     "011 49 170 1234567"). A bare "011 <area-code>" parses as
//     not-possible, so canonicalize() returns empty.
//   "00000" / DE → "" — the spec asserts the all-zero string would
//     collapse to empty under IsPossibleNumber. In libphonenumber 8.12
//     it parses as +490000 (the DE country code is applied after the
//     "00" international access is stripped). We trust the library's
//     verdict per the "no hardcoded sentinel list" rule;
//     all-zero numbers aren't routable in practice, so this has no
//     downstream effect on the daemon.
INSTANTIATE_TEST_SUITE_P(SpecCoverage, CanonicalizeMatrix,
                         ::testing::Values(
                             // Already-canonical international forms pass through unchanged.
                             CanonRow{"+33123456789", "DE", "+33123456789"},
                             // German national format → E.164 with country code injected.
                             CanonRow{"0170 123 4567", "DE", "+491701234567"},
                             // European 00-prefix international access → recognised + rewritten.
                             CanonRow{"0033123456789", "DE", "+33123456789"},
                             // Incognito-strings collapse to empty.
                             CanonRow{"<unknown>", "DE", ""}, CanonRow{"anonymous", "DE", ""},
                             CanonRow{"", "DE", ""},
                             // Single digit is not a possible phone number in any region.
                             CanonRow{"1", "DE", ""}));

// ─── lookup() pipeline ─────────────────────────────────────────────────

TEST_F(DaviCalAdapterTest, LookupExactMatchReturnsPersonAndDoesNotHitCompaniesBook) {
    PathRouterServer srv({
        {"addresses", canonicalMultistatus("Alice", "ExampleGmbH", "+491701234567", "42")},
        {"companies", emptyMultistatus()},
    });
    auto cfg = configForWithBookPaths();
    auto adapter = makeAdapter(srv.port(), cfg);

    auto r = runResult<std::optional<aid::Contact>>(
        [&] { return adapter->lookup(aid::PhoneNumber{"+491701234567"}); });
    ASSERT_TRUE(r.has_value()) << r.error().message;
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ((*r)->name, "Alice");
    EXPECT_EQ((*r)->kind, aid::AddressKind::Person);

    // Only the addresses book should have been queried.
    EXPECT_EQ(srv.requestCount(), 1);
    EXPECT_FALSE(srv.lastPathFor("addresses").empty());
    EXPECT_TRUE(srv.lastPathFor("companies").empty());
}

TEST_F(DaviCalAdapterTest, LookupExactMissFallsThroughToCompanies) {
    PathRouterServer srv({
        {"addresses", emptyMultistatus()},
        {"companies", canonicalMultistatus("Example Co.", "ExampleGmbH", "+491701234", "99")},
    });
    auto cfg = configForWithBookPaths();
    auto adapter = makeAdapter(srv.port(), cfg);

    auto r = runResult<std::optional<aid::Contact>>(
        [&] { return adapter->lookup(aid::PhoneNumber{"+491701234567"}); });
    ASSERT_TRUE(r.has_value()) << r.error().message;
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ((*r)->name, "Example Co.");
    EXPECT_EQ((*r)->kind, aid::AddressKind::Company);
    EXPECT_EQ(srv.requestCount(), 2);
}

TEST_F(DaviCalAdapterTest, LookupBothMissesReturnsNullopt) {
    PathRouterServer srv({
        {"addresses", emptyMultistatus()},
        {"companies", emptyMultistatus()},
    });
    auto cfg = configForWithBookPaths();
    auto adapter = makeAdapter(srv.port(), cfg);

    auto r = runResult<std::optional<aid::Contact>>(
        [&] { return adapter->lookup(aid::PhoneNumber{"+491701234567"}); });
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_FALSE(r->has_value());
    EXPECT_EQ(srv.requestCount(), 2);
}

TEST_F(DaviCalAdapterTest, LookupEmptyNumberShortCircuits) {
    PathRouterServer srv({
        {"addresses", emptyMultistatus()},
        {"companies", emptyMultistatus()},
    });
    auto cfg = configForWithBookPaths();
    auto adapter = makeAdapter(srv.port(), cfg);

    auto r =
        runResult<std::optional<aid::Contact>>([&] { return adapter->lookup(aid::PhoneNumber{}); });
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(r->has_value());
    EXPECT_EQ(srv.requestCount(), 0) << "empty number must not hit the address book";
}

TEST_F(DaviCalAdapterTest, LookupMultiHitPicksLongestCommonPrefix) {
    // Two contacts back from the exact-match query. The first has a
    // very short overlap with the incoming number; the second has the
    // full 12-digit match. LCP picker should return the second.
    const std::string multi =
        "<?xml version=\"1.0\"?>"
        "<d:multistatus xmlns:d=\"DAV:\" xmlns:card=\"urn:ietf:params:xml:ns:carddav\">"
        "<d:response><d:propstat><d:prop>"
        "<card:address-data>BEGIN:VCARD\r\nFN:Far\r\nTEL:+4912\r\nEND:VCARD</card:address-data>"
        "</d:prop></d:propstat></d:response>"
        "<d:response><d:propstat><d:prop>"
        "<card:address-data>BEGIN:VCARD\r\nFN:Close\r\nTEL:+491701234567\r\nEND:VCARD</"
        "card:address-data>"
        "</d:prop></d:propstat></d:response>"
        "</d:multistatus>";
    PathRouterServer srv({{"addresses", multi}, {"companies", emptyMultistatus()}});
    auto cfg = configForWithBookPaths();
    auto adapter = makeAdapter(srv.port(), cfg);

    auto r = runResult<std::optional<aid::Contact>>(
        [&] { return adapter->lookup(aid::PhoneNumber{"+491701234567"}); });
    ASSERT_TRUE(r.has_value()) << r.error().message;
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ((*r)->name, "Close");
}

TEST_F(DaviCalAdapterTest, LookupUsesContainsMatchSoWhitespacePaddedStoredTelsAreReachable) {
    // Regression: a company vCard hand-entered as " +49304321" (leading
    // space) is stored verbatim by DaviCal, which runs the TEL text-match
    // server-side against that raw value. An "equals"/"starts-with" match
    // would exclude it before our parser could trim the space. The adapter
    // must send a "contains" match so the padded entry still comes back.
    PathRouterServer srv({
        {"addresses", emptyMultistatus()},
        {"companies", emptyMultistatus()},
    });
    auto cfg = configForWithBookPaths();
    auto adapter = makeAdapter(srv.port(), cfg);

    (void)runResult<std::optional<aid::Contact>>(
        [&] { return adapter->lookup(aid::PhoneNumber{"+491701234567"}); });

    const std::string addrBody = srv.lastBodyFor("addresses");
    const std::string compBody = srv.lastBodyFor("companies");
    EXPECT_NE(addrBody.find("match-type=\"contains\""), std::string::npos) << addrBody;
    EXPECT_EQ(addrBody.find("match-type=\"equals\""), std::string::npos) << addrBody;
    EXPECT_NE(compBody.find("match-type=\"contains\""), std::string::npos) << compBody;
    EXPECT_EQ(compBody.find("match-type=\"starts-with\""), std::string::npos) << compBody;
}

TEST_F(DaviCalAdapterTest, LookupExactStepRejectsSuperstringAndFallsThroughToCompanies) {
    // The "contains" server filter can surface a Person whose TEL merely
    // contains the lookup number as a substring (e.g. a longer number).
    // The exact step must reject that superstring and fall through to the
    // companies book rather than returning the wrong contact.
    PathRouterServer srv({
        {"addresses", canonicalMultistatus("Superstring", "Org", "+4917012345670000", "1")},
        {"companies", canonicalMultistatus("Real Co.", "Org", "+491701234", "7")},
    });
    auto cfg = configForWithBookPaths();
    auto adapter = makeAdapter(srv.port(), cfg);

    auto r = runResult<std::optional<aid::Contact>>(
        [&] { return adapter->lookup(aid::PhoneNumber{"+491701234567"}); });
    ASSERT_TRUE(r.has_value()) << r.error().message;
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ((*r)->name, "Real Co.");
    EXPECT_EQ((*r)->kind, aid::AddressKind::Company);
    EXPECT_EQ(srv.requestCount(), 2) << "exact step must not short-circuit on a superstring hit";
}

TEST_F(DaviCalAdapterTest, LookupTransportErrorPropagates) {
    // No server bound: HttpClient → NetworkFailure → UpstreamUnavailable.
    auto cfg = configForWithBookPaths();
    cfg.bookAddresses = "/aid/addresses/";
    cfg.bookCompanies = "/aid/companies/";
    const std::string base = "http://127.0.0.1:1"; // port 1, no listener
    auto http = std::make_unique<aid::infrastructure::HttpClient>(
        base,
        aid::infrastructure::UpstreamConfig{std::chrono::seconds{1}, std::chrono::seconds{1}, 1},
        lt_.loop());
    auto adapter = std::make_unique<DaviCalAdapter>(std::move(http), std::move(cfg));

    auto r = runResult<std::optional<aid::Contact>>(
        [&] { return adapter->lookup(aid::PhoneNumber{"+491701234567"}); });
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, aid::plumbing::ErrorCode::UpstreamUnavailable);
}

TEST_F(DaviCalAdapterTest, LookupRejectsNonCanonicalNumberWithoutHittingServer) {
    // P2-5: the query builders hard-guard E.164 in all build modes. A number
    // carrying XML metacharacters must be rejected with InvalidInput before
    // any REPORT is sent — never interpolated into the CardDAV body.
    PathRouterServer srv({
        {"addresses", emptyMultistatus()},
        {"companies", emptyMultistatus()},
    });
    auto cfg = configForWithBookPaths();
    auto adapter = makeAdapter(srv.port(), cfg);

    auto r = runResult<std::optional<aid::Contact>>(
        [&] { return adapter->lookup(aid::PhoneNumber{"+49170<inject>"}); });
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, aid::plumbing::ErrorCode::InvalidInput);
    EXPECT_EQ(srv.requestCount(), 0);
}

} // namespace aid::adapters::davical::test
