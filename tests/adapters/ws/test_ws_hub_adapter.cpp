#include <gtest/gtest.h>

#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>

#include "FakeWebSocketConnection.h"
#include "aid/adapters/ws/WsHubAdapter.h"
#include "aid/crosscutting/Logger.h"
#include "aid/plumbing/ActionResult.h"
#include "aid/value-types/Dashboard.h"
#include "aid/value-types/Ids.h"

using aid::TicketId;
using aid::UserHandle;
using aid::adapters::ws::WsHubAdapter;
using aid::crosscutting::Logger;
using aid::fakes::FakeWebSocketConnection;
using aid::plumbing::ActionResult;

namespace {

struct LoggerOnce {
    LoggerOnce() {
        static std::once_flag flag;
        std::call_once(flag, [] {
            Logger::initialize(aid::crosscutting::LogLevel::ERROR,
                               "/tmp/aid_ws_hub_test_backend.log",
                               "/tmp/aid_ws_hub_test_frontend.log");
        });
    }
};

[[nodiscard]] std::shared_ptr<FakeWebSocketConnection> makeConn() {
    return std::make_shared<FakeWebSocketConnection>();
}

[[nodiscard]] UserHandle uh(std::string v) {
    return UserHandle{std::move(v)};
}

class WsHubAdapterTest : public ::testing::Test {
protected:
    LoggerOnce loggerOnce;
    WsHubAdapter hub{Logger::instance()};
};

} // namespace

TEST_F(WsHubAdapterTest, OnConnectAccepts) {
    auto conn = makeConn();
    EXPECT_TRUE(hub.onConnect(uh("alice"), conn));
    EXPECT_EQ(hub.subscriberCount(), 1u);
}

TEST_F(WsHubAdapterTest, OnConnectGroupsByUser) {
    auto c1 = makeConn();
    auto c2 = makeConn();
    EXPECT_TRUE(hub.onConnect(uh("alice"), c1));
    EXPECT_TRUE(hub.onConnect(uh("alice"), c2));
    EXPECT_EQ(hub.subscriberCount(), 2u);
}

TEST_F(WsHubAdapterTest, OnConnectAt500ReturnsFalse) {
    std::vector<std::shared_ptr<FakeWebSocketConnection>> keep;
    for (std::size_t i = 0; i < WsHubAdapter::MAX_SUBSCRIBERS; ++i) {
        auto c = makeConn();
        keep.push_back(c);
        ASSERT_TRUE(hub.onConnect(uh("u" + std::to_string(i % 10)), c));
    }
    ASSERT_EQ(hub.subscriberCount(), WsHubAdapter::MAX_SUBSCRIBERS);
    auto overflow = makeConn();
    EXPECT_FALSE(hub.onConnect(uh("zoe"), overflow));
    EXPECT_EQ(hub.subscriberCount(), WsHubAdapter::MAX_SUBSCRIBERS);
}

TEST_F(WsHubAdapterTest, OnDisconnectRemovesConnection) {
    auto c1 = makeConn();
    auto c2 = makeConn();
    ASSERT_TRUE(hub.onConnect(uh("alice"), c1));
    ASSERT_TRUE(hub.onConnect(uh("alice"), c2));
    hub.onDisconnect(c1);
    EXPECT_EQ(hub.subscriberCount(), 1u);
    // Second disconnect of the same conn is a no-op.
    hub.onDisconnect(c1);
    EXPECT_EQ(hub.subscriberCount(), 1u);
}

TEST_F(WsHubAdapterTest, OnDisconnectOfUnknownIsNoOp) {
    auto known = makeConn();
    auto unknown = makeConn();
    ASSERT_TRUE(hub.onConnect(uh("alice"), known));
    hub.onDisconnect(unknown);
    EXPECT_EQ(hub.subscriberCount(), 1u);
}

TEST_F(WsHubAdapterTest, NotifyInvalidateBroadcastsToAll) {
    auto a1 = makeConn();
    auto a2 = makeConn();
    auto b1 = makeConn();
    ASSERT_TRUE(hub.onConnect(uh("alice"), a1));
    ASSERT_TRUE(hub.onConnect(uh("alice"), a2));
    ASSERT_TRUE(hub.onConnect(uh("bob"), b1));

    hub.notifyInvalidate("dashboard");

    EXPECT_EQ(a1->sentCount(), 1u);
    EXPECT_EQ(a2->sentCount(), 1u);
    EXPECT_EQ(b1->sentCount(), 1u);

    const auto j = nlohmann::json::parse(a1->sent().at(0));
    EXPECT_EQ(j.at("type"), "invalidate");
    EXPECT_EQ(j.at("scope"), "dashboard");
}

TEST_F(WsHubAdapterTest, NotifyInvalidateUserTargetsOneUser) {
    auto a1 = makeConn();
    auto b1 = makeConn();
    ASSERT_TRUE(hub.onConnect(uh("alice"), a1));
    ASSERT_TRUE(hub.onConnect(uh("bob"), b1));

    hub.notifyInvalidateUser(uh("alice"), "ticket:42");

    EXPECT_EQ(a1->sentCount(), 1u);
    EXPECT_EQ(b1->sentCount(), 0u);
    const auto j = nlohmann::json::parse(a1->sent().at(0));
    EXPECT_EQ(j.at("type"), "invalidate");
    EXPECT_EQ(j.at("scope"), "ticket:42");
}

TEST_F(WsHubAdapterTest, NotifyActionResultTargetsOneUser) {
    auto a1 = makeConn();
    auto b1 = makeConn();
    ASSERT_TRUE(hub.onConnect(uh("alice"), a1));
    ASSERT_TRUE(hub.onConnect(uh("bob"), b1));

    ActionResult r;
    r.ok = true;
    r.op = "COMMENT_SAVE";
    r.ticketId = TicketId{"1234"};
    r.message = std::string{"saved"};
    hub.notifyActionResult(uh("alice"), r);

    EXPECT_EQ(a1->sentCount(), 1u);
    EXPECT_EQ(b1->sentCount(), 0u);
    const auto j = nlohmann::json::parse(a1->sent().at(0));
    EXPECT_EQ(j.at("type"), "action_result");
    EXPECT_EQ(j.at("op"), "COMMENT_SAVE");
    EXPECT_EQ(j.at("ticketId"), "1234");
    EXPECT_EQ(j.at("ok"), true);
    EXPECT_EQ(j.at("message"), "saved");
}

TEST_F(WsHubAdapterTest, NotifyActionResultAbsentMessageSerializesAsNull) {
    auto a1 = makeConn();
    ASSERT_TRUE(hub.onConnect(uh("alice"), a1));

    ActionResult r;
    r.ok = false;
    r.op = "TICKET_CLOSE";
    r.ticketId = TicketId{"99"};
    // r.message left as nullopt — must serialize as JSON null, matching the
    // REST ActionResult serialization in UiController (one rule for the FE).
    hub.notifyActionResult(uh("alice"), r);

    const auto j = nlohmann::json::parse(a1->sent().at(0));
    EXPECT_EQ(j.at("ok"), false);
    EXPECT_TRUE(j.at("message").is_null());
}

TEST_F(WsHubAdapterTest, NotifyInvalidateAfterDisconnectSkipsDeparted) {
    auto a1 = makeConn();
    auto a2 = makeConn();
    ASSERT_TRUE(hub.onConnect(uh("alice"), a1));
    ASSERT_TRUE(hub.onConnect(uh("alice"), a2));
    hub.onDisconnect(a1);

    hub.notifyInvalidate("dashboard");

    EXPECT_EQ(a1->sentCount(), 0u);
    EXPECT_EQ(a2->sentCount(), 1u);
}

TEST_F(WsHubAdapterTest, SubscriberCountReflectsState) {
    EXPECT_EQ(hub.subscriberCount(), 0u);
    auto c1 = makeConn();
    auto c2 = makeConn();
    ASSERT_TRUE(hub.onConnect(uh("alice"), c1));
    EXPECT_EQ(hub.subscriberCount(), 1u);
    ASSERT_TRUE(hub.onConnect(uh("bob"), c2));
    EXPECT_EQ(hub.subscriberCount(), 2u);
    hub.onDisconnect(c1);
    EXPECT_EQ(hub.subscriberCount(), 1u);
    hub.onDisconnect(c2);
    EXPECT_EQ(hub.subscriberCount(), 0u);
}

TEST_F(WsHubAdapterTest, EmptyUserHandleStoresUnderEmptyBucket) {
    auto anon = makeConn();
    auto alice = makeConn();
    ASSERT_TRUE(hub.onConnect(uh(""), anon));
    ASSERT_TRUE(hub.onConnect(uh("alice"), alice));

    hub.notifyInvalidate("dashboard");
    EXPECT_EQ(anon->sentCount(), 1u);
    EXPECT_EQ(alice->sentCount(), 1u);

    hub.notifyInvalidateUser(uh("alice"), "ticket:7");
    EXPECT_EQ(anon->sentCount(), 1u);  // unchanged
    EXPECT_EQ(alice->sentCount(), 2u); // got the second
}

TEST_F(WsHubAdapterTest, OnConnectNullPointerRejected) {
    EXPECT_FALSE(hub.onConnect(uh("alice"), nullptr));
    EXPECT_EQ(hub.subscriberCount(), 0u);
}

TEST_F(WsHubAdapterTest, PushTicketUpsertTargetsOneUserAndCarriesEntryPlusVersion) {
    auto a1 = makeConn();
    auto b1 = makeConn();
    ASSERT_TRUE(hub.onConnect(uh("alice"), a1));
    ASSERT_TRUE(hub.onConnect(uh("bob"), b1));

    aid::DashboardEntry e;
    e.id = TicketId{"4242"};
    e.subject = "Acme GmbH";
    e.status = aid::TicketStatus::InProgress;
    e.statusId = aid::StatusId{"7"};
    e.href = "https://op.example/projects/support/work_packages/4242";
    e.projectName = "support";
    e.lockVersion = 5;
    hub.pushTicketUpsert(uh("alice"), e);

    EXPECT_EQ(a1->sentCount(), 1u);
    EXPECT_EQ(b1->sentCount(), 0u);
    const auto j = nlohmann::json::parse(a1->sent().at(0));
    EXPECT_EQ(j.at("type"), "ticket_upsert");
    // Version rides at the frame top level so the viewer can drop a stale frame.
    EXPECT_EQ(j.at("lockVersion"), 5);
    // The embedded entry is the shared REST projection (same keys, byte-identical).
    const auto& entry = j.at("entry");
    EXPECT_EQ(entry.at("id"), "4242");
    EXPECT_EQ(entry.at("subject"), "Acme GmbH");
    EXPECT_EQ(entry.at("status"), "InProgress");
    EXPECT_EQ(entry.at("statusId"), "7");
    EXPECT_EQ(entry.at("href"), "https://op.example/projects/support/work_packages/4242");
    // lockVersion is intentionally NOT duplicated inside entry (REST contract).
    EXPECT_FALSE(entry.contains("lockVersion"));
    // updatedAt DOES ride inside the entry — it is the frontend's sort key for
    // re-ordering merged deltas (ISO-8601 UTC, serialized by aid_serialization).
    EXPECT_TRUE(entry.at("updatedAt").is_string());
}

TEST_F(WsHubAdapterTest, PushTicketRemoveTargetsOneUserWithIdAndVersion) {
    auto a1 = makeConn();
    auto b1 = makeConn();
    ASSERT_TRUE(hub.onConnect(uh("alice"), a1));
    ASSERT_TRUE(hub.onConnect(uh("bob"), b1));

    hub.pushTicketRemove(uh("alice"), TicketId{"99"}, 12);

    EXPECT_EQ(a1->sentCount(), 1u);
    EXPECT_EQ(b1->sentCount(), 0u);
    const auto j = nlohmann::json::parse(a1->sent().at(0));
    EXPECT_EQ(j.at("type"), "ticket_remove");
    EXPECT_EQ(j.at("ticketId"), "99");
    EXPECT_EQ(j.at("lockVersion"), 12);
}
