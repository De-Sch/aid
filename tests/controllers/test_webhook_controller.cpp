#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <gtest/gtest.h>
#include <trantor/net/EventLoop.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include "aid/controllers/WebhookController.h"
#include "aid/crosscutting/Clock.h"
#include "aid/crosscutting/CorrelationId.h"
#include "aid/crosscutting/Logger.h"
#include "aid/infrastructure/Wal.h"
#include "aid/infrastructure/WebhookMailbox.h"
#include "aid/plumbing/Result.h"
#include "aid/plumbing/Task.h"
#include "aid/value-types/Ids.h"

namespace {

using aid::controllers::WebhookController;
using aid::crosscutting::Clock;
using aid::crosscutting::CorrelationId;
using aid::crosscutting::Logger;
using aid::infrastructure::Wal;
using aid::infrastructure::WebhookMailbox;
using aid::plumbing::Result;
using aid::plumbing::Task;

constexpr const char* kSecret = "s3cr3t-shared-key";

class FixedClock final : public Clock {
public:
    [[nodiscard]] aid::Timestamp now() const override {
        return aid::Timestamp{std::chrono::milliseconds{1'700'000'000'000}};
    }
};

// Sync that always fails — exercises the WAL-sync 500 branch (mirror of the
// CallController test's FaultySyncWal).
class FaultySyncWal final : public Wal {
public:
    using Wal::Wal;

protected:
    int syncToDisk(int /*fd*/) noexcept override {
        errno = EIO;
        return -1;
    }
};

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

struct LoggerOnce {
    LoggerOnce() {
        static std::once_flag flag;
        std::call_once(flag, [] {
            const auto tmp = std::filesystem::temp_directory_path();
            Logger::initialize(aid::crosscutting::LogLevel::ERROR,
                               (tmp / "aid_webhook_ctrl_test_backend.log").string(),
                               (tmp / "aid_webhook_ctrl_test_frontend.log").string());
        });
    }
};

class WebhookControllerTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto pid = static_cast<std::uint64_t>(::getpid());
        static std::atomic<std::uint64_t> counter{0};
        const auto n = counter.fetch_add(1, std::memory_order_relaxed);
        dir_ = std::filesystem::temp_directory_path() /
               ("aid_webhook_ctrl_test_" + std::to_string(pid) + "_" + std::to_string(n));
        std::filesystem::create_directories(dir_);
        walPath_ = (dir_ / "webhook.log").string();
        wal_ = std::make_unique<Wal>(walPath_, clock_);
    }

    void TearDown() override {
        mb_.reset();
        wal_.reset();
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    static WebhookMailbox::Handler noopHandler() {
        return [](std::string, std::string) -> Task<Result<void>> { co_return Result<void>{}; };
    }

    void makeMailbox(WebhookMailbox::Handler h = noopHandler()) {
        mb_ = std::make_unique<WebhookMailbox>(loopThread_.loop(), *wal_, Logger::instance(),
                                               std::move(h), &WebhookController::ticketIdOf);
    }

    drogon::HttpRequestPtr makeRequest(std::string body, std::optional<std::string> secretHeader) {
        auto req = drogon::HttpRequest::newHttpRequest();
        req->setMethod(drogon::Post);
        req->setBody(std::move(body));
        if (secretHeader) {
            req->addHeader("X-AID-Webhook-Secret", *secretHeader);
        }
        return req;
    }

    LoggerOnce loggerInit_{};
    FixedClock clock_{};
    CorrelationId cid_{};
    std::filesystem::path dir_;
    std::string walPath_;
    std::unique_ptr<Wal> wal_;
    LoopThread loopThread_;
    std::unique_ptr<WebhookMailbox> mb_;
};

drogon::HttpStatusCode invoke(WebhookController& c, drogon::HttpRequestPtr req) {
    std::optional<drogon::HttpStatusCode> status;
    c.handlePost(req, [&](const drogon::HttpResponsePtr& resp) { status = resp->getStatusCode(); });
    EXPECT_TRUE(status.has_value());
    return status.value_or(drogon::k500InternalServerError);
}

std::string envelope(int id, int lockVersion) {
    return R"({"action":"work_package:updated","work_package":{"id":)" + std::to_string(id) +
           R"(,"lockVersion":)" + std::to_string(lockVersion) + R"(,"subject":"x"}})";
}

// ---- ticketIdOf: key extraction ----

TEST(WebhookControllerKey, ExtractsIdFromEnvelope) {
    auto id = WebhookController::ticketIdOf(envelope(42, 3));
    ASSERT_TRUE(id.has_value());
    EXPECT_EQ(id->v, "42");
}

TEST(WebhookControllerKey, ExtractsIdFromBareHalWorkPackage) {
    auto id = WebhookController::ticketIdOf(R"({"id":7,"lockVersion":1,"_links":{}})");
    ASSERT_TRUE(id.has_value());
    EXPECT_EQ(id->v, "7");
}

TEST(WebhookControllerKey, AcceptsStringId) {
    auto id = WebhookController::ticketIdOf(R"({"work_package":{"id":"99"}})");
    ASSERT_TRUE(id.has_value());
    EXPECT_EQ(id->v, "99");
}

TEST(WebhookControllerKey, MissingIdYieldsNullopt) {
    EXPECT_FALSE(WebhookController::ticketIdOf(R"({"work_package":{"subject":"x"}})").has_value());
    EXPECT_FALSE(WebhookController::ticketIdOf(R"({"action":"ping"})").has_value());
}

TEST(WebhookControllerKey, GarbageJsonYieldsNullopt) {
    EXPECT_FALSE(WebhookController::ticketIdOf("not json").has_value());
    EXPECT_FALSE(WebhookController::ticketIdOf("[]").has_value());
    EXPECT_FALSE(WebhookController::ticketIdOf("").has_value());
}

// ---- handlePost: secret gate + WAL + enqueue ----

TEST_F(WebhookControllerTest, HappyPath_ValidSecret_WritesWalReturns202) {
    makeMailbox();
    WebhookController c{*wal_, *mb_, Logger::instance(), cid_, kSecret};

    auto req = makeRequest(envelope(42, 3), kSecret);
    EXPECT_EQ(invoke(c, req), drogon::k202Accepted);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (std::chrono::steady_clock::now() < deadline) {
        if (wal_->pendingCount() == 0 && mb_->liveCount() == 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    EXPECT_EQ(mb_->liveCount(), 0U);
}

TEST_F(WebhookControllerTest, WrongSecret_Returns401_NoWal) {
    makeMailbox();
    WebhookController c{*wal_, *mb_, Logger::instance(), cid_, kSecret};

    auto req = makeRequest(envelope(42, 3), std::string{"wrong-key"});
    EXPECT_EQ(invoke(c, req), drogon::k401Unauthorized);

    std::ifstream f(walPath_);
    std::string contents((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    EXPECT_TRUE(contents.empty()) << "rejected request must not touch the WAL";
}

TEST_F(WebhookControllerTest, AbsentSecret_Returns401) {
    makeMailbox();
    WebhookController c{*wal_, *mb_, Logger::instance(), cid_, kSecret};

    auto req = makeRequest(envelope(42, 3), std::nullopt);
    EXPECT_EQ(invoke(c, req), drogon::k401Unauthorized);
}

TEST_F(WebhookControllerTest, SecretViaQueryParameter_Returns202) {
    makeMailbox();
    WebhookController c{*wal_, *mb_, Logger::instance(), cid_, kSecret};

    auto req = makeRequest(envelope(5, 1), std::nullopt);
    req->setParameter("secret", kSecret);
    EXPECT_EQ(invoke(c, req), drogon::k202Accepted);
}

TEST_F(WebhookControllerTest, ValidSecretButNoTicketId_Returns400) {
    makeMailbox();
    WebhookController c{*wal_, *mb_, Logger::instance(), cid_, kSecret};

    auto req =
        makeRequest(R"({"action":"work_package:updated","work_package":{"subject":"x"}})", kSecret);
    EXPECT_EQ(invoke(c, req), drogon::k400BadRequest);
}

TEST_F(WebhookControllerTest, WalSyncFailure_Returns500_NoEnqueue) {
    FaultySyncWal faultyWal{walPath_, clock_};
    WebhookMailbox mb{loopThread_.loop(), faultyWal, Logger::instance(), noopHandler(),
                      &WebhookController::ticketIdOf};
    WebhookController c{faultyWal, mb, Logger::instance(), cid_, kSecret};

    auto req = makeRequest(envelope(42, 3), kSecret);
    EXPECT_EQ(invoke(c, req), drogon::k500InternalServerError);
    EXPECT_EQ(mb.liveCount(), 0U);
}

TEST_F(WebhookControllerTest, DrainingMailbox_Returns503) {
    makeMailbox();
    auto drainTask = mb_->drain(std::chrono::seconds{1});
    ASSERT_TRUE(drainTask.done());

    WebhookController c{*wal_, *mb_, Logger::instance(), cid_, kSecret};
    auto req = makeRequest(envelope(42, 3), kSecret);
    EXPECT_EQ(invoke(c, req), drogon::k503ServiceUnavailable);
}

} // namespace
