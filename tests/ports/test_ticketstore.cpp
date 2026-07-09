#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "aid/plumbing/Error.h"
#include "aid/plumbing/Result.h"
#include "aid/plumbing/Task.h"
#include "aid/ports/TicketStore.h"
#include "aid/value-types/Dashboard.h"
#include "aid/value-types/Ids.h"
#include "aid/value-types/Ticket.h"

namespace {

using aid::CallId;
using aid::DashboardEntry;
using aid::NewTicket;
using aid::PhoneNumber;
using aid::ProjectId;
using aid::Ticket;
using aid::TicketId;
using aid::UserHandle;
using aid::plumbing::Error;
using aid::plumbing::ErrorCode;
using aid::plumbing::Result;
using aid::plumbing::Task;
using aid::plumbing::unexpected;
using aid::ports::TicketStore;

static_assert(std::is_abstract_v<TicketStore>);
static_assert(std::has_virtual_destructor_v<TicketStore>);
static_assert(!std::is_copy_constructible_v<TicketStore>);
static_assert(!std::is_move_constructible_v<TicketStore>);

class StubTicketStore final : public TicketStore {
public:
    Task<Result<Ticket>> fetchById(TicketId id) override {
        Ticket t{};
        t.id = id;
        co_return Result<Ticket>{t};
    }
    Task<Result<std::optional<Ticket>>> findByExactCallid(CallId) override {
        co_return Result<std::optional<Ticket>>{std::nullopt};
    }
    Task<Result<std::optional<Ticket>>> findByCallidContains(CallId) override {
        co_return Result<std::optional<Ticket>>{std::nullopt};
    }
    Task<Result<std::optional<Ticket>>> findLatestOpenCallInProject(ProjectId) override {
        co_return Result<std::optional<Ticket>>{std::nullopt};
    }
    Task<Result<std::optional<Ticket>>> findOpenInProjectBySubject(ProjectId,
                                                                   std::string_view) override {
        co_return Result<std::optional<Ticket>>{std::nullopt};
    }
    Task<Result<std::optional<Ticket>>> findOpenInProjectByCallerNumber(ProjectId,
                                                                        PhoneNumber) override {
        co_return Result<std::optional<Ticket>>{std::nullopt};
    }
    Task<Result<std::optional<UserHandle>>> resolveUser(std::string_view) override {
        co_return Result<std::optional<UserHandle>>{std::nullopt};
    }
    Task<Result<std::vector<ProjectId>>> listProjectsForUser(UserHandle) override {
        co_return Result<std::vector<ProjectId>>{std::vector<ProjectId>{}};
    }
    Task<Result<std::vector<DashboardEntry>>> listDashboard(UserHandle) override {
        co_return Result<std::vector<DashboardEntry>>{std::vector<DashboardEntry>{}};
    }
    DashboardEntry buildEntry(const Ticket& t, UserHandle) override {
        DashboardEntry e;
        e.id = t.id;
        return e;
    }
    Task<Result<TicketId>> create(const NewTicket&) override {
        co_return Result<TicketId>{TicketId{"new-1"}};
    }
    Task<Result<Ticket>> save(TicketId, aid::ports::TicketReducer) override {
        co_return Result<Ticket>{unexpected{Error{ErrorCode::Conflict409, "lockVersion stale",
                                                  std::optional<std::string>{"cid-stub"}}}};
    }
    Task<Result<void>> addCallHandler(TicketId, UserHandle) override { co_return Result<void>{}; }
    Task<Result<std::vector<UserHandle>>> recipientsFor(const Ticket&) override {
        co_return Result<std::vector<UserHandle>>{std::vector<UserHandle>{}};
    }
    Task<Result<std::vector<Ticket>>> openCallsInProject(ProjectId) override {
        co_return Result<std::vector<Ticket>>{std::vector<Ticket>{}};
    }
    Task<Result<std::vector<aid::MembershipDelta>>> refreshMembership() override {
        co_return Result<std::vector<aid::MembershipDelta>>{std::vector<aid::MembershipDelta>{}};
    }
    Task<Result<void>> close(TicketId) override { co_return Result<void>{}; }
    Task<Result<std::optional<aid::WebhookDecode>>> decodeWebhook(std::string) override {
        co_return Result<std::optional<aid::WebhookDecode>>{std::nullopt};
    }
    Task<Result<void>> ping() override { co_return Result<void>{}; }
};

TEST(TicketStorePort, FetchByIdDispatchesThroughBase) {
    std::unique_ptr<TicketStore> store = std::make_unique<StubTicketStore>();
    auto task = store->fetchById(TicketId{"42"});
    ASSERT_TRUE(task.done());
    auto result = task.await_resume();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().id.v, "42");
}

TEST(TicketStorePort, SavePropagatesConflict409Error) {
    std::unique_ptr<TicketStore> store = std::make_unique<StubTicketStore>();
    auto task = store->save(TicketId{"42"}, [](Ticket t) { return t; });
    ASSERT_TRUE(task.done());
    auto result = task.await_resume();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::Conflict409);
    ASSERT_TRUE(result.error().correlationId.has_value());
    EXPECT_EQ(*result.error().correlationId, "cid-stub");
}

TEST(TicketStorePort, CloseReturnsSuccessVoid) {
    std::unique_ptr<TicketStore> store = std::make_unique<StubTicketStore>();
    auto task = store->close(TicketId{"7"});
    ASSERT_TRUE(task.done());
    auto result = task.await_resume();
    EXPECT_TRUE(result.has_value());
}

} // namespace
