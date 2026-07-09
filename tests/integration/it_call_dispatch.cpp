#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <gtest/gtest.h>
#include <trantor/net/EventLoop.h>
#include <unistd.h>

#include <atomic>
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

#include "FakeAddressBook.h"
#include "FakeClock.h"
#include "FakeTicketStore.h"
#include "FakeUiNotifier.h"
#include "aid/controllers/CallController.h"
#include "aid/crosscutting/Clock.h"
#include "aid/crosscutting/Config.h"
#include "aid/crosscutting/CorrelationId.h"
#include "aid/crosscutting/Logger.h"
#include "aid/infrastructure/Mailbox.h"
#include "aid/infrastructure/Wal.h"
#include "aid/plumbing/Result.h"
#include "aid/plumbing/Task.h"
#include "aid/usecases/HandleAcceptedCall.h"
#include "aid/usecases/HandleHangup.h"
#include "aid/usecases/HandleIncomingCall.h"
#include "aid/usecases/HandleOutgoingCall.h"
#include "aid/usecases/HandleTransferCall.h"
#include "aid/value-types/CallEvent.h"
#include "aid/value-types/Contact.h"
#include "aid/value-types/Ids.h"
#include "aid/value-types/Ticket.h"

namespace {

using aid::AcceptedCall;
using aid::CallId;
using aid::Contact;
using aid::HangupCall;
using aid::IncomingCall;
using aid::OutgoingCall;
using aid::PhoneNumber;
using aid::ProjectId;
using aid::Ticket;
using aid::TicketId;
using aid::TicketStatus;
using aid::TransferCall;
using aid::UserHandle;
using aid::controllers::CallController;
using aid::crosscutting::Config;
using aid::crosscutting::CorrelationId;
using aid::crosscutting::Logger;
using aid::fakes::FakeAddressBook;
using aid::fakes::FakeClock;
using aid::fakes::FakeTicketStore;
using aid::fakes::FakeUiNotifier;
using aid::infrastructure::Mailbox;
using aid::infrastructure::Wal;
using aid::plumbing::Result;
using aid::plumbing::Task;
using aid::usecases::HandleAcceptedCall;
using aid::usecases::HandleHangup;
using aid::usecases::HandleIncomingCall;
using aid::usecases::HandleOutgoingCall;
using aid::usecases::HandleTransferCall;

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
                               (tmp / "aid_integration_backend.log").string(),
                               (tmp / "aid_integration_frontend.log").string());
        });
    }
};

class CallDispatchE2E : public ::testing::Test {
protected:
    void SetUp() override {
        const auto pid = static_cast<std::uint64_t>(::getpid());
        static std::atomic<std::uint64_t> counter{0};
        const auto n = counter.fetch_add(1, std::memory_order_relaxed);
        dir_ = std::filesystem::temp_directory_path() /
               ("aid_dispatch_" + std::to_string(pid) + "_" + std::to_string(n));
        std::filesystem::create_directories(dir_);
        walPath_ = (dir_ / "inbox.log").string();
        wal_ = std::make_unique<Wal>(walPath_, clock_);

        incoming_ = std::make_unique<HandleIncomingCall>(ts_, ab_, ui_, fakeClock_, cfg_);
        outgoing_ = std::make_unique<HandleOutgoingCall>(ts_, ab_, ui_, fakeClock_, cfg_);
        accepted_ = std::make_unique<HandleAcceptedCall>(ts_, ui_, fakeClock_);
        transfer_ = std::make_unique<HandleTransferCall>(ts_, ui_);
        hangup_ = std::make_unique<HandleHangup>(ts_, ui_, fakeClock_);

        Mailbox::Handlers handlers;
        handlers.incoming = [this](const IncomingCall& ev, bool replay) -> Task<Result<void>> {
            co_return co_await incoming_->run(ev, replay);
        };
        handlers.outgoing = [this](const OutgoingCall& ev, bool replay) -> Task<Result<void>> {
            co_return co_await outgoing_->run(ev, replay);
        };
        handlers.accepted = [this](const AcceptedCall& ev) -> Task<Result<void>> {
            co_return co_await accepted_->run(ev);
        };
        handlers.transfer = [this](const TransferCall& ev) -> Task<Result<void>> {
            co_return co_await transfer_->run(ev);
        };
        handlers.hangup = [this](const HangupCall& ev) -> Task<Result<void>> {
            co_return co_await hangup_->run(ev);
        };
        mb_ = std::make_unique<Mailbox>(loopThread_.loop(), *wal_, Logger::instance(),
                                        std::move(handlers), nullptr);
        controller_ = std::make_unique<CallController>(*wal_, *mb_, Logger::instance(), cid_);
    }

    void TearDown() override {
        controller_.reset();
        mb_.reset();
        incoming_.reset();
        outgoing_.reset();
        accepted_.reset();
        transfer_.reset();
        hangup_.reset();
        wal_.reset();
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    drogon::HttpStatusCode invoke(const std::string& body) {
        auto req = drogon::HttpRequest::newHttpRequest();
        req->setMethod(drogon::Post);
        req->setBody(body);
        std::optional<drogon::HttpStatusCode> status;
        controller_->handlePost(
            req, [&](const drogon::HttpResponsePtr& resp) { status = resp->getStatusCode(); });
        EXPECT_TRUE(status.has_value());
        return status.value_or(drogon::k500InternalServerError);
    }

    template <class Pred>
    bool waitUntil(Pred&& p, std::chrono::milliseconds budget = std::chrono::milliseconds{2000}) {
        const auto deadline = std::chrono::steady_clock::now() + budget;
        while (std::chrono::steady_clock::now() < deadline) {
            if (p()) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        return false;
    }

    LoggerOnce loggerInit_{};
    FakeClock clock_;
    FakeClock fakeClock_;
    CorrelationId cid_{};
    Config::TicketRouting cfg_{ProjectId{"FB"}, "Incognito Caller"};
    FakeTicketStore ts_;
    FakeAddressBook ab_;
    FakeUiNotifier ui_;
    std::filesystem::path dir_;
    std::string walPath_;
    std::unique_ptr<Wal> wal_;
    LoopThread loopThread_;
    std::unique_ptr<HandleIncomingCall> incoming_;
    std::unique_ptr<HandleOutgoingCall> outgoing_;
    std::unique_ptr<HandleAcceptedCall> accepted_;
    std::unique_ptr<HandleTransferCall> transfer_;
    std::unique_ptr<HandleHangup> hangup_;
    std::unique_ptr<Mailbox> mb_;
    std::unique_ptr<CallController> controller_;
};

// All four c1-lifecycle events (Incoming → Accepted → Transfer → Hangup)
// are dispatched to their respective usecases through one Mailbox. Each
// step's side effect is observed before posting the next, so the fake
// deques pop in the exact order the usecases call them.
TEST_F(CallDispatchE2E, IncomingAcceptedTransferHangup_AllRouteCorrectly) {
    // --- Incoming (callid c1) ---
    ab_.canonicalizeMap["+491701234567"] = "+491701234567";
    Contact c;
    c.name = "Alice";
    c.projectIds = {ProjectId{"P1"}};
    ab_.nextLookup.push_back(std::optional<Contact>{c});
    ts_.nextFindOpenInProjectByCallerNumber.push_back(std::optional<Ticket>{});
    ts_.nextCreate.push_back(TicketId{"T-c1"});
    // Incoming's live-delta path: re-fetch by id, then fan a ticket_upsert to
    // recipientsFor. (The later Accepted/Transfer/Hangup steps also emit, but
    // their fetchById/recipientsFor are intentionally left unstubbed — those
    // emits no-op best-effort, keeping this test focused on routing/state.)
    Ticket post_create;
    post_create.id = TicketId{"T-c1"};
    post_create.projectId = ProjectId{"P1"};
    post_create.status = TicketStatus::New;
    ts_.nextFetchById.push_back(Result<Ticket>{post_create});
    ts_.nextRecipientsFor.push_back(
        Result<std::vector<UserHandle>>{std::vector<UserHandle>{UserHandle{"alice"}}});

    EXPECT_EQ(invoke(R"({"event":"Incoming Call","remote":"+491701234567",)"
                     R"("callid":"c1","dialed":"+4930"})"),
              drogon::k202Accepted);
    ASSERT_TRUE(waitUntil([&] { return ts_.created.size() == 1 && ui_.ticketUpserts.size() == 1; }))
        << "Incoming must route to HandleIncomingCall, create a ticket, and push a delta";
    EXPECT_EQ(ts_.created[0].callId, CallId{"c1"});
    EXPECT_EQ(ts_.created[0].projectId, ProjectId{"P1"});
    EXPECT_EQ(ui_.ticketUpserts[0].second.id, TicketId{"T-c1"});

    // --- Accepted (callid c1) ---
    // Simulate the ticket state that the OpenProject adapter would have
    // after Incoming: T-c1 with callid c1, no callStart yet.
    Ticket post_incoming;
    post_incoming.id = TicketId{"T-c1"};
    post_incoming.projectId = ProjectId{"P1"};
    post_incoming.subject = "Alice";
    post_incoming.status = TicketStatus::New;
    post_incoming.callIds = {CallId{"c1"}};
    ts_.nextFindByCallidContains.push_back(std::optional<Ticket>{post_incoming});
    ts_.nextResolveUser.push_back(std::optional<UserHandle>{UserHandle{"alice"}});
    ts_.nextSave.push_back(post_incoming);
    ts_.nextAddCallHandler.push_back(Result<void>{});

    fakeClock_.now_ = aid::Timestamp{} + std::chrono::hours(24 * 365 * 56);
    EXPECT_EQ(invoke(R"({"event":"Accepted Call","callid":"c1",)"
                     R"("remote":"+491701234567","dialed":"+4930","user":"alice"})"),
              drogon::k202Accepted);
    ASSERT_TRUE(waitUntil([&] { return ts_.saved.size() == 1; }))
        << "Accepted must route to HandleAcceptedCall and save the ticket";
    EXPECT_EQ(ts_.saved[0].status, TicketStatus::InProgress);
    ASSERT_TRUE(ts_.saved[0].assignee.has_value());
    EXPECT_EQ(ts_.saved[0].assignee->v, "alice");
    ASSERT_TRUE(ts_.saved[0].callStart.has_value());
    EXPECT_NE(ts_.saved[0].callLength.find("(c1)"), std::string::npos);
    EXPECT_NE(ts_.saved[0].callLength.find("alice:"), std::string::npos);

    // --- Transfer (callid c1) ---
    Ticket post_accepted = ts_.saved[0];
    ts_.nextFindByCallidContains.push_back(std::optional<Ticket>{post_accepted});
    ts_.nextResolveUser.push_back(std::optional<UserHandle>{UserHandle{"bob"}});
    ts_.nextSave.push_back(post_accepted);
    ts_.nextAddCallHandler.push_back(Result<void>{});

    EXPECT_EQ(invoke(R"({"event":"Transfer Call","callid":"c1","newuser":"bob"})"),
              drogon::k202Accepted);
    ASSERT_TRUE(waitUntil([&] { return ts_.saved.size() == 2; }))
        << "Transfer must route to HandleTransferCall and save";
    // The ticket already had an assignee (alice from accept); OpenProject allows
    // a single assignee, so transfer preserves it and records bob in the
    // callHandler CSV instead — bob stays visible without churning the assignee.
    ASSERT_TRUE(ts_.saved[1].assignee.has_value());
    EXPECT_EQ(ts_.saved[1].assignee->v, "alice");
    ASSERT_GE(ts_.addCallHandler_args.size(), 2U);
    EXPECT_EQ(ts_.addCallHandler_args.back().second.v, "bob");
    EXPECT_NE(ts_.saved[1].callLength.find("bob:"), std::string::npos);
    EXPECT_EQ(ts_.saved[1].callLength.find("alice:"), std::string::npos)
        << "Transfer rewrote alice's prefix to bob's";

    // --- Hangup (callid c1) ---
    Ticket post_transfer = ts_.saved[1];
    ts_.nextFindByCallidContains.push_back(std::optional<Ticket>{post_transfer});
    ts_.nextSave.push_back(post_transfer);

    fakeClock_.now_ =
        aid::Timestamp{} + std::chrono::hours(24 * 365 * 56) + std::chrono::minutes(7);
    EXPECT_EQ(invoke(R"({"event":"Hangup","callid":"c1","remote":"+491701234567"})"),
              drogon::k202Accepted);
    ASSERT_TRUE(waitUntil([&] { return ts_.saved.size() == 3; }))
        << "Hangup must route to HandleHangup and save";
    EXPECT_TRUE(ts_.saved[2].callIds.empty()) << "Hangup removed c1 from the callIds list";
    ASSERT_TRUE(ts_.saved[2].callEnd.has_value());
    EXPECT_NE(ts_.saved[2].callLength.find("Call End:"), std::string::npos);
    EXPECT_EQ(ts_.saved[2].callLength.find("Duration:"), std::string::npos);
    EXPECT_EQ(ts_.saved[2].callLength.find("(c1)"), std::string::npos)
        << "Hangup completes the line and strips the now-useless callid (it was "
           "present on the open line after accept — see the (c1) check above)";

    // Pin: WAL must be fully truncated after all 4 successful runs.
    ASSERT_TRUE(waitUntil([&] {
        std::ifstream f(walPath_);
        std::string contents((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        return contents.empty();
    })) << "WAL must be empty after all 4 lifecycle events succeed";
}

// Outgoing on its own callid — proves the 5th event variant routes to
// HandleOutgoingCall. Kept separate from the c1 lifecycle to avoid
// racing fake-deque pops between parallel mailboxes.
TEST_F(CallDispatchE2E, Outgoing_RoutesToOutgoingUsecase) {
    ts_.nextResolveUser.push_back(std::optional<UserHandle>{UserHandle{"alice"}});
    ab_.canonicalizeMap["+491701234567"] = "+491701234567";
    Contact c;
    c.name = "Bob";
    c.projectIds = {ProjectId{"P1"}};
    ab_.nextLookup.push_back(std::optional<Contact>{c});
    ts_.nextFindOpenInProjectByCallerNumber.push_back(std::optional<Ticket>{});
    ts_.nextCreate.push_back(TicketId{"T-c2"});
    ts_.nextAddCallHandler.push_back(Result<void>{});

    EXPECT_EQ(invoke(R"({"event":"Outgoing Call","callid":"c2",)"
                     R"("remote":"+491701234567","user":"alice"})"),
              drogon::k202Accepted);
    ASSERT_TRUE(waitUntil([&] { return ts_.created.size() == 1; }))
        << "Outgoing must route to HandleOutgoingCall and create a ticket";
    EXPECT_EQ(ts_.created[0].callId, CallId{"c2"});
    EXPECT_EQ(ts_.created[0].projectId, ProjectId{"P1"});
    ASSERT_TRUE(ts_.created[0].assignee.has_value());
    EXPECT_EQ(ts_.created[0].assignee->v, "alice");
    EXPECT_FALSE(ts_.created[0].calledNumber.has_value())
        << "Outgoing carries no dialed/calledNumber";
}

} // namespace
