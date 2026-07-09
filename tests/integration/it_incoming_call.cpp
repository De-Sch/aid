#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
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
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <variant>

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
#include "aid/usecases/HandleIncomingCall.h"
#include "aid/value-types/CallEvent.h"
#include "aid/value-types/Contact.h"
#include "aid/value-types/Ids.h"
#include "aid/value-types/Ticket.h"

namespace {

using aid::CallId;
using aid::Contact;
using aid::IncomingCall;
using aid::PhoneNumber;
using aid::ProjectId;
using aid::Ticket;
using aid::TicketId;
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
using aid::usecases::HandleIncomingCall;

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

class IncomingCallE2E : public ::testing::Test {
protected:
    void SetUp() override {
        const auto pid = static_cast<std::uint64_t>(::getpid());
        static std::atomic<std::uint64_t> counter{0};
        const auto n = counter.fetch_add(1, std::memory_order_relaxed);
        dir_ = std::filesystem::temp_directory_path() /
               ("aid_integration_" + std::to_string(pid) + "_" + std::to_string(n));
        std::filesystem::create_directories(dir_);
        walPath_ = (dir_ / "inbox.log").string();
        wal_ = std::make_unique<Wal>(walPath_, clock_);

        useCase_ = std::make_unique<HandleIncomingCall>(ts_, ab_, ui_, fakeClock_, cfg_);
        Mailbox::Handlers handlers;
        handlers.incoming = [this](const IncomingCall& ev, bool replay) -> Task<Result<void>> {
            auto r = co_await useCase_->run(ev, replay);
            co_return r;
        };
        handlers.outgoing = [](const aid::OutgoingCall&, bool) -> Task<Result<void>> {
            co_return Result<void>{};
        };
        handlers.accepted = [](const aid::AcceptedCall&) -> Task<Result<void>> {
            co_return Result<void>{};
        };
        handlers.transfer = [](const aid::TransferCall&) -> Task<Result<void>> {
            co_return Result<void>{};
        };
        handlers.hangup = [](const aid::HangupCall&) -> Task<Result<void>> {
            co_return Result<void>{};
        };
        mb_ = std::make_unique<Mailbox>(loopThread_.loop(), *wal_, Logger::instance(),
                                        std::move(handlers), nullptr);
        controller_ = std::make_unique<CallController>(*wal_, *mb_, Logger::instance(), cid_);
    }

    void TearDown() override {
        controller_.reset();
        mb_.reset();
        useCase_.reset();
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
    aid::infrastructure::Wal* walPtr_{nullptr};
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
    std::unique_ptr<HandleIncomingCall> useCase_;
    std::unique_ptr<Mailbox> mb_;
    std::unique_ptr<CallController> controller_;
};

TEST_F(IncomingCallE2E, KnownCaller_PostThroughControllerCreatesTicket) {
    ab_.canonicalizeMap["+491701234567"] = "+491701234567";
    Contact c;
    c.name = "Alice";
    c.projectIds = {ProjectId{"P1"}};
    ab_.nextLookup.push_back(std::optional<Contact>{c});
    ts_.nextFindOpenInProjectByCallerNumber.push_back(std::optional<Ticket>{});
    ts_.nextCreate.push_back(TicketId{"T-created"});
    // After create the use case re-fetches by id and emits a per-recipient
    // ticket_upsert (the fresh New ticket).
    Ticket created;
    created.id = TicketId{"T-created"};
    created.status = aid::TicketStatus::New;
    ts_.nextFetchById.push_back(Result<Ticket>{created});
    ts_.nextRecipientsFor.push_back(Result<std::vector<aid::UserHandle>>{
        std::vector<aid::UserHandle>{aid::UserHandle{"alice"}}});

    const auto status = invoke(
        R"({"event":"Incoming Call","remote":"+491701234567","callid":"e2e-1","dialed":"+4930"})");
    EXPECT_EQ(status, drogon::k202Accepted);

    ASSERT_TRUE(
        waitUntil([&] { return ts_.created.size() == 1 && ui_.ticketUpserts.size() == 1; }));
    EXPECT_EQ(ts_.created[0].projectId, ProjectId{"P1"});
    EXPECT_EQ(ts_.created[0].subject, "Alice");
    EXPECT_EQ(ts_.created[0].callerNumber.v, "+491701234567");
    EXPECT_EQ(ui_.ticketUpserts[0].second.id, TicketId{"T-created"});

    // After successful handler, the worker truncates the WAL entry.
    ASSERT_TRUE(waitUntil([&] {
        std::ifstream f(walPath_);
        std::string contents((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        return contents.empty();
    })) << "WAL must be truncated after the use case succeeds";
}

TEST_F(IncomingCallE2E, IncognitoCaller_AlwaysCreatesFreshTicketWithIncognitoCaller) {
    ab_.defaultEmpty = true; // canonicalize → empty triggers incognito branch
    ts_.nextCreate.push_back(TicketId{"T-incognito"});

    const auto status = invoke(
        R"({"event":"Incoming Call","remote":"<unknown>","callid":"e2e-2","dialed":"+4930"})");
    EXPECT_EQ(status, drogon::k202Accepted);

    ASSERT_TRUE(waitUntil([&] { return ts_.created.size() == 1; }));
    EXPECT_EQ(ts_.created[0].projectId, ProjectId{"FB"});
    EXPECT_EQ(ts_.created[0].subject, "Incognito Caller");
    EXPECT_EQ(ts_.created[0].callerNumber.v, "Incognito");
    // Incognito is never deduped: no by-subject lookup happens.
    EXPECT_TRUE(ts_.findOpenInProjectBySubject_args.empty());
}

TEST_F(IncomingCallE2E, SameCallid_TwoPostsSerializedAndBothProcessed) {
    ab_.canonicalizeMap["+491701234567"] = "+491701234567";
    Contact c;
    c.name = "Alice";
    c.projectIds = {ProjectId{"P1"}};
    ab_.nextLookup.push_back(std::optional<Contact>{c});
    ab_.nextLookup.push_back(std::optional<Contact>{c});

    // First event: no open ticket → create.
    ts_.nextFindOpenInProjectByCallerNumber.push_back(std::optional<Ticket>{});
    ts_.nextCreate.push_back(TicketId{"T-first"});
    // Second event with the same callid: open ticket now exists (we'd
    // simulate that with a save). Since the fakes are stateless, just hand
    // it another fresh create slot — the test verifies "both events
    // processed in order through one worker", not the OpenProject mutation
    // graph.
    ts_.nextFindOpenInProjectByCallerNumber.push_back(std::optional<Ticket>{});
    ts_.nextCreate.push_back(TicketId{"T-second"});

    const std::string body =
        R"({"event":"Incoming Call","remote":"+491701234567","callid":"serial","dialed":"+4930"})";
    EXPECT_EQ(invoke(body), drogon::k202Accepted);
    EXPECT_EQ(invoke(body), drogon::k202Accepted);

    ASSERT_TRUE(waitUntil([&] { return ts_.created.size() == 2; }));
    EXPECT_EQ(ts_.created[0].callId, CallId{"serial"});
    EXPECT_EQ(ts_.created[1].callId, CallId{"serial"});
}

} // namespace
