#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/HttpTypes.h>
#include <gtest/gtest.h>
#include <trantor/net/EventLoop.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>

#include "../fakes/FakeAddressBook.h"
#include "../fakes/FakeTicketStore.h"
#include "aid/controllers/HealthController.h"
#include "aid/crosscutting/Clock.h"
#include "aid/crosscutting/Logger.h"
#include "aid/infrastructure/HealthService.h"
#include "aid/infrastructure/Mailbox.h"
#include "aid/infrastructure/Wal.h"
#include "aid/plumbing/Result.h"
#include "aid/plumbing/Task.h"
#include "aid/value-types/Ids.h"

namespace {

using aid::Timestamp;
using aid::controllers::HealthController;
using aid::crosscutting::Clock;
using aid::fakes::FakeAddressBook;
using aid::fakes::FakeTicketStore;
using aid::infrastructure::HealthService;
using aid::infrastructure::Mailbox;
using aid::infrastructure::Wal;
using aid::plumbing::Result;
using aid::plumbing::Task;

class FixedClock final : public Clock {
public:
    [[nodiscard]] Timestamp now() const override {
        return Timestamp{std::chrono::milliseconds{1'700'000'000'000}};
    }
};

class LoopThread {
public:
    LoopThread() {
        std::promise<trantor::EventLoop*> ready;
        auto fut = ready.get_future();
        thread_ = std::thread([this, &ready] {
            trantor::EventLoop loop;
            ready.set_value(&loop);
            loop.loop();
        });
        loop_ = fut.get();
    }
    LoopThread(const LoopThread&) = delete;
    LoopThread& operator=(const LoopThread&) = delete;
    LoopThread(LoopThread&&) = delete;
    LoopThread& operator=(LoopThread&&) = delete;
    ~LoopThread() {
        if (loop_) {
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
            const auto tmp = std::filesystem::temp_directory_path();
            aid::crosscutting::Logger::initialize(
                aid::crosscutting::LogLevel::ERROR,
                (tmp / "aid_health_ctrl_test_backend.log").string(),
                (tmp / "aid_health_ctrl_test_frontend.log").string());
        });
    }
};

class HealthControllerTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto pid = static_cast<std::uint64_t>(::getpid());
        static std::atomic<std::uint64_t> counter{0};
        const auto n = counter.fetch_add(1, std::memory_order_relaxed);
        dir_ = std::filesystem::temp_directory_path() /
               ("aid_health_ctrl_test_" + std::to_string(pid) + "_" + std::to_string(n));
        std::filesystem::create_directories(dir_);
        walPath_ = (dir_ / "inbox.log").string();
        wal_ = std::make_unique<Wal>(walPath_, clock_);

        auto noop = [](auto&&...) -> Task<Result<void>> { co_return Result<void>{}; };
        Mailbox::Handlers handlers{noop, noop, noop, noop, noop};
        mailbox_ =
            std::make_unique<Mailbox>(loop_.loop(), *wal_, aid::crosscutting::Logger::instance(),
                                      std::move(handlers), nullptr);
        svc_ = std::make_unique<HealthService>(ts_, ab_, *mailbox_, /*pluginsLoaded=*/true);
        ctrl_ = std::make_unique<HealthController>(*svc_);
    }
    void TearDown() override {
        ctrl_.reset();
        svc_.reset();
        mailbox_.reset();
        wal_.reset();
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    [[nodiscard]] static drogon::HttpRequestPtr getRequest() {
        auto req = drogon::HttpRequest::newHttpRequest();
        req->setMethod(drogon::Get);
        req->setPath("/health");
        return req;
    }

    LoggerOnce logger_init_{};
    FixedClock clock_{};
    LoopThread loop_{};
    std::filesystem::path dir_;
    std::string walPath_;
    std::unique_ptr<Wal> wal_;
    std::unique_ptr<Mailbox> mailbox_;
    FakeTicketStore ts_{};
    FakeAddressBook ab_{};
    std::unique_ptr<HealthService> svc_;
    std::unique_ptr<HealthController> ctrl_;
};

TEST_F(HealthControllerTest, Get_Returns_200_Json) {
    drogon::HttpResponsePtr resp;
    ctrl_->get(getRequest(), [&](const drogon::HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), drogon::k200OK);
    // Content-Type now lives in Drogon's dedicated field (set via
    // setContentTypeString), so read it with contentTypeString(), not
    // getHeader() — and the wire carries exactly one Content-Type.
    EXPECT_EQ(resp->contentTypeString(), "application/json");
}

TEST_F(HealthControllerTest, Body_HasAllSevenFields) {
    drogon::HttpResponsePtr resp;
    ctrl_->get(getRequest(), [&](const drogon::HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);

    const auto j = nlohmann::json::parse(resp->getBody());
    ASSERT_TRUE(j.is_object());
    EXPECT_TRUE(j.contains("status"));
    EXPECT_TRUE(j["status"].is_string());
    EXPECT_TRUE(j.contains("pluginsLoaded"));
    EXPECT_TRUE(j["pluginsLoaded"].is_boolean());
    EXPECT_TRUE(j.contains("ticketSystem"));
    EXPECT_TRUE(j["ticketSystem"].is_string());
    EXPECT_TRUE(j.contains("addressSystem"));
    EXPECT_TRUE(j["addressSystem"].is_string());
    EXPECT_TRUE(j.contains("uptimeS"));
    EXPECT_TRUE(j["uptimeS"].is_number_integer());
    EXPECT_TRUE(j.contains("queuedEvents"));
    EXPECT_TRUE(j["queuedEvents"].is_number_unsigned());
    EXPECT_TRUE(j.contains("failedEvents"));
    EXPECT_TRUE(j["failedEvents"].is_number_unsigned());
}

TEST_F(HealthControllerTest, Body_ReflectsBootstrapResult) {
    ts_.nextPing.push_back(Result<void>{});
    ab_.nextPing.push_back(Result<void>{});
    auto t = svc_->bootstrapPing();
    ASSERT_TRUE(t.done());

    drogon::HttpResponsePtr resp;
    ctrl_->get(getRequest(), [&](const drogon::HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    const auto j = nlohmann::json::parse(resp->getBody());
    EXPECT_EQ(j["status"].get<std::string>(), "ok");
    EXPECT_EQ(j["ticketSystem"].get<std::string>(), "reachable");
    EXPECT_EQ(j["addressSystem"].get<std::string>(), "reachable");
    EXPECT_TRUE(j["pluginsLoaded"].get<bool>());
}

TEST_F(HealthControllerTest, NoUpstreamCallsFromHandler) {
    // pre-condition: no bootstrap ping issued — counters are zero.
    EXPECT_EQ(ts_.ping_calls, 0);
    EXPECT_EQ(ab_.ping_calls, 0);

    drogon::HttpResponsePtr resp;
    ctrl_->get(getRequest(), [&](const drogon::HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);

    // post-condition: handler reads cached snap only — still zero.
    EXPECT_EQ(ts_.ping_calls, 0);
    EXPECT_EQ(ab_.ping_calls, 0);

    const auto j = nlohmann::json::parse(resp->getBody());
    EXPECT_EQ(j["status"].get<std::string>(), "starting");
}

} // namespace
