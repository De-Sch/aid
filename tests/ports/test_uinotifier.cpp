#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <string_view>
#include <type_traits>

#include "aid/plumbing/ActionResult.h"
#include "aid/ports/UiNotifier.h"
#include "aid/value-types/Dashboard.h"
#include "aid/value-types/Ids.h"

namespace {

using aid::TicketId;
using aid::UserHandle;
using aid::plumbing::ActionResult;
using aid::ports::UiNotifier;

static_assert(std::is_abstract_v<UiNotifier>);
static_assert(std::has_virtual_destructor_v<UiNotifier>);
static_assert(!std::is_copy_constructible_v<UiNotifier>);
static_assert(!std::is_move_constructible_v<UiNotifier>);

class StubUiNotifier final : public UiNotifier {
public:
    int invalidateCalls = 0;
    int invalidateUserCalls = 0;
    int actionResultCalls = 0;
    std::string lastScope;
    std::string lastUserScope;
    UserHandle lastUser;
    ActionResult lastResult;

    void notifyInvalidate(std::string_view scope) override {
        ++invalidateCalls;
        lastScope = std::string{scope};
    }
    void notifyInvalidateUser(UserHandle user, std::string_view scope) override {
        ++invalidateUserCalls;
        lastUser = user;
        lastUserScope = std::string{scope};
    }
    void notifyActionResult(UserHandle user, const ActionResult& result) override {
        ++actionResultCalls;
        lastUser = user;
        lastResult = result;
    }
    void pushTicketUpsert(UserHandle user, const aid::DashboardEntry& entry) override {
        ++upsertCalls;
        lastUser = user;
        lastUpsertId = entry.id.v;
        lastUpsertLockVersion = entry.lockVersion;
    }
    void pushTicketRemove(UserHandle user, aid::TicketId ticketId, int lockVersion) override {
        ++removeCalls;
        lastUser = user;
        lastRemoveId = ticketId.v;
        lastRemoveLockVersion = lockVersion;
    }

    int upsertCalls = 0;
    int removeCalls = 0;
    std::string lastUpsertId;
    int lastUpsertLockVersion = 0;
    std::string lastRemoveId;
    int lastRemoveLockVersion = 0;
};

TEST(UiNotifierPort, DispatchesAllThreeMethodsThroughBase) {
    auto owned = std::make_unique<StubUiNotifier>();
    StubUiNotifier* probe = owned.get();
    std::unique_ptr<UiNotifier> hub = std::move(owned);

    hub->notifyInvalidate("dashboard");
    hub->notifyInvalidateUser(UserHandle{"alice"}, "ticket:42");
    ActionResult r{};
    r.ok = true;
    r.op = "COMMENT_SAVE";
    r.ticketId = TicketId{"42"};
    hub->notifyActionResult(UserHandle{"alice"}, r);

    EXPECT_EQ(probe->invalidateCalls, 1);
    EXPECT_EQ(probe->lastScope, "dashboard");
    EXPECT_EQ(probe->invalidateUserCalls, 1);
    EXPECT_EQ(probe->lastUserScope, "ticket:42");
    EXPECT_EQ(probe->lastUser.v, "alice");
    EXPECT_EQ(probe->actionResultCalls, 1);
    EXPECT_TRUE(probe->lastResult.ok);
    EXPECT_EQ(probe->lastResult.op, "COMMENT_SAVE");
    EXPECT_EQ(probe->lastResult.ticketId.v, "42");
}

} // namespace
