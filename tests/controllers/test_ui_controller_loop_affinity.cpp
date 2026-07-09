#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/HttpTypes.h>
#include <gtest/gtest.h>
#include <trantor/net/EventLoop.h>
#include <trantor/net/EventLoopThread.h>

#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "FakeAddressBook.h"
#include "FakeUiNotifier.h"
#include "aid/controllers/SessionGuard.h"
#include "aid/controllers/UiController.h"
#include "aid/crosscutting/CorrelationId.h"
#include "aid/crosscutting/Logger.h"
#include "aid/plumbing/Error.h"
#include "aid/plumbing/Result.h"
#include "aid/plumbing/Task.h"
#include "aid/ports/TicketStore.h"
#include "aid/usecases/AppendComment.h"
#include "aid/usecases/CloseTicket.h"
#include "aid/usecases/GetDashboard.h"
#include "aid/value-types/Dashboard.h"
#include "aid/value-types/Ids.h"

// Regression test for the loop-affinity bug. The /ui/* handlers are AsyncTasks
// started on the connection's listener loop, but the upstream co_await resumes
// them on the dedicated domain loop. Before the fix, the final cb(response) ran
// on the domain loop — a foreign loop for that connection (UB).
// After the fix the handler captures its loop at entry and `co_await
// resumeOn(connLoop)` hops back before cb. Here we drive a UiController across
// two real trantor loops and assert cb is invoked on the loop that "accepted"
// the request (acceptance criterion 2 of issue 0002).

namespace {

using aid::plumbing::Error;
using aid::plumbing::ErrorCode;
using aid::plumbing::Result;
using aid::plumbing::Task;
using aid::plumbing::unexpected;

[[nodiscard]] Error stubError() {
    return Error{ErrorCode::InvariantViolation, "LoopAffinityStore: method unused in this test",
                 std::nullopt};
}

// Minimal TicketStore whose listDashboard hops onto a chosen loop (mimicking an
// upstream HTTP call bound to the domain loop) before returning. Every other
// method is unused by the no-active-call dashboard path and returns an error.
class LoopAffinityStore final : public aid::ports::TicketStore {
public:
    trantor::EventLoop* hopTo = nullptr;
    std::vector<aid::DashboardEntry> entries;

    Task<Result<std::vector<aid::DashboardEntry>>> listDashboard(aid::UserHandle) override {
        // Resume on the "domain" loop, exactly as an HttpClient bound to it
        // would — the GetDashboard continuation then runs on hopTo, not the
        // connection loop, forcing the controller to hop back before cb.
        co_await aid::plumbing::resumeOn(hopTo);
        co_return entries;
    }

    Task<Result<aid::Ticket>> fetchById(aid::TicketId) override {
        co_return unexpected{stubError()};
    }
    Task<Result<std::optional<aid::Ticket>>> findByExactCallid(aid::CallId) override {
        co_return unexpected{stubError()};
    }
    Task<Result<std::optional<aid::Ticket>>> findByCallidContains(aid::CallId) override {
        co_return unexpected{stubError()};
    }
    Task<Result<std::optional<aid::Ticket>>> findLatestOpenCallInProject(aid::ProjectId) override {
        co_return unexpected{stubError()};
    }
    Task<Result<std::optional<aid::Ticket>>> findOpenInProjectBySubject(aid::ProjectId,
                                                                        std::string_view) override {
        co_return unexpected{stubError()};
    }
    Task<Result<std::optional<aid::Ticket>>>
    findOpenInProjectByCallerNumber(aid::ProjectId, aid::PhoneNumber) override {
        co_return unexpected{stubError()};
    }
    Task<Result<std::optional<aid::UserHandle>>> resolveUser(std::string_view) override {
        co_return unexpected{stubError()};
    }
    Task<Result<std::vector<aid::ProjectId>>> listProjectsForUser(aid::UserHandle) override {
        co_return unexpected{stubError()};
    }
    aid::DashboardEntry buildEntry(const aid::Ticket&, aid::UserHandle) override { return {}; }
    Task<Result<aid::TicketId>> create(const aid::NewTicket&) override {
        co_return unexpected{stubError()};
    }
    Task<Result<aid::Ticket>> save(aid::TicketId, aid::ports::TicketReducer) override {
        co_return unexpected{stubError()};
    }
    Task<Result<void>> addCallHandler(aid::TicketId, aid::UserHandle) override {
        co_return unexpected{stubError()};
    }
    Task<Result<std::vector<aid::UserHandle>>> recipientsFor(const aid::Ticket&) override {
        co_return unexpected{stubError()};
    }
    Task<Result<std::vector<aid::Ticket>>> openCallsInProject(aid::ProjectId) override {
        co_return unexpected{stubError()};
    }
    Task<Result<std::vector<aid::MembershipDelta>>> refreshMembership() override {
        co_return unexpected{stubError()};
    }
    Task<Result<void>> close(aid::TicketId) override { co_return unexpected{stubError()}; }
    Task<Result<std::optional<aid::WebhookDecode>>> decodeWebhook(std::string) override {
        co_return unexpected{stubError()};
    }
    Task<Result<void>> ping() override { co_return unexpected{stubError()}; }
};

struct LoggerOnce {
    LoggerOnce() {
        static std::once_flag flag;
        std::call_once(flag, [] {
            aid::crosscutting::Logger::initialize(aid::crosscutting::LogLevel::ERROR,
                                                  "/tmp/aid_ui_loop_test_backend.log",
                                                  "/tmp/aid_ui_loop_test_frontend.log");
        });
    }
};

} // namespace

TEST(UiControllerLoopAffinity, DashboardCallbackRunsOnConnectionLoop) {
    LoggerOnce loggerOnce;

    // Two real loops: D stands in for the dedicated domain loop, L for the
    // listener loop that accepts the request.
    trantor::EventLoopThread domainThread{"domain"};
    trantor::EventLoopThread listenerThread{"listener"};
    domainThread.run();
    listenerThread.run();
    trantor::EventLoop* const domainLoop = domainThread.getLoop();
    trantor::EventLoop* const listenerLoop = listenerThread.getLoop();
    ASSERT_NE(domainLoop, nullptr);
    ASSERT_NE(listenerLoop, nullptr);
    ASSERT_NE(domainLoop, listenerLoop);

    LoopAffinityStore ts;
    ts.hopTo = domainLoop;
    aid::DashboardEntry e; // no activeCallForViewer → no address-book lookup
    e.id = aid::TicketId{"1"};
    e.href = "https://op.example/projects/support/work_packages/1";
    ts.entries.push_back(e);

    aid::fakes::FakeAddressBook ab;
    aid::fakes::FakeUiNotifier ui;
    aid::crosscutting::CorrelationId cid;
    aid::usecases::GetDashboard dashboard{ts, ab};
    aid::usecases::AppendComment comment{ts, ui};
    aid::usecases::CloseTicket close{ts, ui};
    aid::controllers::UiController ctrl{dashboard, comment, close, cid,
                                        aid::crosscutting::Logger::instance()};

    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Get);
    req->attributes()->insert(aid::controllers::SessionGuard::VIEWER_KEY, aid::UserHandle{"alice"});

    std::promise<trantor::EventLoop*> cbLoopPromise;
    auto cbLoopFuture = cbLoopPromise.get_future();

    // Invoke the handler ON the listener loop, exactly as Drogon would, so that
    // getEventLoopOfCurrentThread() inside the handler captures listenerLoop.
    listenerLoop->queueInLoop([&]() {
        ctrl.getDashboard(req, [&cbLoopPromise](const drogon::HttpResponsePtr& resp) {
            EXPECT_NE(resp, nullptr);
            cbLoopPromise.set_value(trantor::EventLoop::getEventLoopOfCurrentThread());
        });
    });

    auto* const observed = cbLoopFuture.get();
    EXPECT_EQ(observed, listenerLoop)
        << "response callback must run on the connection's loop, not the domain loop";
}
