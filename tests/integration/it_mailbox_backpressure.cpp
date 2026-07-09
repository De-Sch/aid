#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/HttpTypes.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "IntegrationHarness.h"
#include "aid/controllers/CallController.h"
#include "aid/crosscutting/Clock.h"
#include "aid/crosscutting/CorrelationId.h"
#include "aid/crosscutting/Logger.h"
#include "aid/infrastructure/Mailbox.h"
#include "aid/infrastructure/Wal.h"
#include "aid/plumbing/Error.h"
#include "aid/plumbing/Result.h"
#include "aid/plumbing/Task.h"
#include "aid/value-types/CallEvent.h"
#include "aid/value-types/Ids.h"

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

// CallController → Mailbox backpressure: when the per-callid deque is at
// MAX_QUEUE the controller must respond 503 rather than 202. Mirrors the
// shape of the existing Mailbox unit-test (tests/infrastructure/
// test_mailbox.cpp::Queue_Full_Returns_Error) but drives the path through
// the production HTTP entry point so the 503 mapping itself
// (CallController.cpp:160-167) is in the contract.
class MailboxBackpressureE2E : public ::testing::Test {
protected:
    void SetUp() override {
        const auto pid = static_cast<std::uint64_t>(::getpid());
        static std::atomic<std::uint64_t> counter{0};
        const auto n = counter.fetch_add(1, std::memory_order_relaxed);
        dir_ = std::filesystem::temp_directory_path() /
               ("aid_bp_" + std::to_string(pid) + "_" + std::to_string(n));
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

    drogon::HttpStatusCode invokePost(CallController& controller, const std::string& body) {
        auto req = drogon::HttpRequest::newHttpRequest();
        req->setMethod(drogon::Post);
        req->setBody(body);
        std::optional<drogon::HttpStatusCode> status;
        controller.handlePost(
            req, [&status](const drogon::HttpResponsePtr& r) { status = r->getStatusCode(); });
        EXPECT_TRUE(status.has_value());
        return status.value_or(drogon::k500InternalServerError);
    }

    LoggerOnce loggerInit_{};
    FakeClock clock_;
    CorrelationId cid_;
    std::filesystem::path dir_;
    std::string walPath_;
    std::unique_ptr<Wal> wal_;
};

} // namespace

// Direct Mailbox::enqueue assertion: 1 in-flight + MAX_QUEUE queued
// succeeds; the next enqueue returns InvalidInput + "mailbox full".
TEST_F(MailboxBackpressureE2E, Mailbox_Enqueue_QueueFull_ReturnsMailboxFullError) {
    LoopThread lt;

    auto promise = std::make_shared<std::promise<void>>();
    auto fut = promise->get_future().share();
    std::atomic<int> entered{0};
    std::atomic<int> dispatched{0};

    auto handlers = noopHandlers();
    handlers.incoming = [fut, &entered, &dispatched](const IncomingCall&,
                                                     bool) -> Task<Result<void>> {
        entered.fetch_add(1, std::memory_order_release);
        // TEST-ONLY synchronous wait — same construct as
        // tests/infrastructure/test_mailbox.cpp::Queue_Full_Returns_Error.
        fut.wait();
        dispatched.fetch_add(1, std::memory_order_relaxed);
        co_return Result<void>{};
    };

    Mailbox mb{lt.loop(), *wal_, Logger::instance(), std::move(handlers), nullptr};

    const CallId hot{"hot"};

    const auto seq0 = *wal_->append("{}", "cid-0");
    ASSERT_TRUE(mb.enqueue(hot,
                           IncomingCall{hot, PhoneNumber{"+491701234567"}, PhoneNumber{"+4930"}},
                           "cid-0", seq0)
                    .has_value());
    ASSERT_TRUE(waitUntil([&] { return entered.load() == 1; }))
        << "worker must enter the blocking handler before we start filling";

    for (std::size_t i = 0; i < Mailbox::MAX_QUEUE; ++i) {
        const auto seq = *wal_->append("{}", "cid-fill");
        ASSERT_TRUE(
            mb.enqueue(hot, IncomingCall{hot, PhoneNumber{"+491701234567"}, PhoneNumber{"+4930"}},
                       "cid-fill", seq)
                .has_value())
            << "fill event " << i;
    }

    const auto seqOver = *wal_->append("{}", "cid-over");
    const auto rejected =
        mb.enqueue(hot, IncomingCall{hot, PhoneNumber{"+491701234567"}, PhoneNumber{"+4930"}},
                   "cid-over", seqOver);
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(rejected.error().code, ErrorCode::InvalidInput);
    EXPECT_EQ(rejected.error().message, "mailbox full");

    promise->set_value();
    ASSERT_TRUE(
        waitUntil([&] { return dispatched.load() == 1 + static_cast<int>(Mailbox::MAX_QUEUE); },
                  std::chrono::milliseconds{10000}));
}

// Same overflow path, but driven through CallController::handlePost. Each
// POST writes to the WAL (fsync) before enqueue, so the test exercises the
// production parse → fsync → enqueue → status-code chain end to end.
TEST_F(MailboxBackpressureE2E, CallController_QueueFull_Returns503) {
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

    const std::string body = incomingJson("hot-call");

    EXPECT_EQ(invokePost(controller, body), drogon::k202Accepted);
    ASSERT_TRUE(waitUntil([&] { return entered.load() == 1; }));

    for (std::size_t i = 0; i < Mailbox::MAX_QUEUE; ++i) {
        EXPECT_EQ(invokePost(controller, body), drogon::k202Accepted)
            << "fill POST " << i << " must accept";
    }

    EXPECT_EQ(invokePost(controller, body), drogon::k503ServiceUnavailable)
        << "the 34th POST exceeds MAX_QUEUE and must be rejected with 503";

    promise->set_value();
}

// Once Mailbox::drain has flipped the draining_ flag every subsequent
// enqueue (and therefore every CallController POST) must return 503 —
// regardless of queue occupancy.
TEST_F(MailboxBackpressureE2E, CallController_AfterDrainFlag_Returns503) {
    LoopThread lt;
    Mailbox mb{lt.loop(), *wal_, Logger::instance(), noopHandlers(), nullptr};

    // Drain a quiescent mailbox: activeWorkers_ is empty, so drain breaks
    // immediately and returns. The draining_ flag stays set for the rest of
    // the mailbox's lifetime, which is the documented one-way SIGTERM-path
    // contract.
    auto t = mb.drain(std::chrono::seconds{0});
    EXPECT_TRUE(t.done());

    CallController controller{*wal_, mb, Logger::instance(), cid_};
    EXPECT_EQ(invokePost(controller, incomingJson("anything")), drogon::k503ServiceUnavailable);
}
