#include <drogon/HttpRequest.h>
#include <drogon/HttpTypes.h>
#include <drogon/WebSocketConnection.h>
#include <gtest/gtest.h>

#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "FakeWebSocketConnection.h"
#include "aid/adapters/ws/WsHubAdapter.h"
#include "aid/controllers/SessionGuard.h"
#include "aid/controllers/UiStreamController.h"
#include "aid/crosscutting/CorrelationId.h"
#include "aid/crosscutting/Logger.h"
#include "aid/value-types/Ids.h"

using aid::UserHandle;
using aid::adapters::ws::WsHubAdapter;
using aid::controllers::SessionGuard;
using aid::controllers::UiStreamController;
using aid::crosscutting::CorrelationId;
using aid::crosscutting::Logger;
using aid::fakes::FakeWebSocketConnection;

namespace {

struct LoggerOnce {
    LoggerOnce() {
        static std::once_flag flag;
        std::call_once(flag, [] {
            Logger::initialize(aid::crosscutting::LogLevel::ERROR,
                               "/tmp/aid_ui_stream_test_backend.log",
                               "/tmp/aid_ui_stream_test_frontend.log");
        });
    }
};

[[nodiscard]] drogon::HttpRequestPtr makeReq(std::optional<UserHandle> viewer) {
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::HttpMethod::Get);
    if (viewer.has_value()) {
        req->attributes()->insert(SessionGuard::VIEWER_KEY, *viewer);
    }
    return req;
}

class UiStreamControllerTest : public ::testing::Test {
protected:
    LoggerOnce loggerOnce;
    CorrelationId cid;
    WsHubAdapter hub{Logger::instance()};
    UiStreamController controller;

    void SetUp() override { UiStreamController::install(hub, Logger::instance(), cid); }
    void TearDown() override { UiStreamController::uninstall(); }
};

} // namespace

TEST_F(UiStreamControllerTest, HandleNewConnectionWithViewerCallsOnConnect) {
    auto conn = std::make_shared<FakeWebSocketConnection>();
    auto req = makeReq(UserHandle{"alice"});

    controller.handleNewConnection(req, conn);

    EXPECT_EQ(hub.subscriberCount(), 1u);
    EXPECT_FALSE(conn->isClosed());
}

TEST_F(UiStreamControllerTest, HandleNewConnectionWithMissingViewerForceCloses) {
    auto conn = std::make_shared<FakeWebSocketConnection>();
    auto req = makeReq(std::nullopt);

    controller.handleNewConnection(req, conn);

    EXPECT_EQ(hub.subscriberCount(), 0u);
    EXPECT_TRUE(conn->isClosed());
}

TEST_F(UiStreamControllerTest, HandleNewConnectionWithEmptyViewerForceCloses) {
    auto conn = std::make_shared<FakeWebSocketConnection>();
    auto req = makeReq(UserHandle{""});

    controller.handleNewConnection(req, conn);

    EXPECT_EQ(hub.subscriberCount(), 0u);
    EXPECT_TRUE(conn->isClosed());
}

TEST_F(UiStreamControllerTest, HandleNewConnectionAtCapForceCloses) {
    std::vector<std::shared_ptr<FakeWebSocketConnection>> keep;
    for (std::size_t i = 0; i < WsHubAdapter::MAX_SUBSCRIBERS; ++i) {
        auto c = std::make_shared<FakeWebSocketConnection>();
        keep.push_back(c);
        ASSERT_TRUE(hub.onConnect(UserHandle{"filler"}, c));
    }
    ASSERT_EQ(hub.subscriberCount(), WsHubAdapter::MAX_SUBSCRIBERS);

    auto conn = std::make_shared<FakeWebSocketConnection>();
    auto req = makeReq(UserHandle{"alice"});

    controller.handleNewConnection(req, conn);

    EXPECT_EQ(hub.subscriberCount(), WsHubAdapter::MAX_SUBSCRIBERS);
    EXPECT_TRUE(conn->isClosed());
}

TEST_F(UiStreamControllerTest, HandleConnectionClosedCallsOnDisconnect) {
    auto conn = std::make_shared<FakeWebSocketConnection>();
    auto req = makeReq(UserHandle{"alice"});
    controller.handleNewConnection(req, conn);
    ASSERT_EQ(hub.subscriberCount(), 1u);

    controller.handleConnectionClosed(conn);

    EXPECT_EQ(hub.subscriberCount(), 0u);
}

TEST_F(UiStreamControllerTest, HandleNewMessageIsNoOp) {
    auto conn = std::make_shared<FakeWebSocketConnection>();
    auto req = makeReq(UserHandle{"alice"});
    controller.handleNewConnection(req, conn);
    ASSERT_EQ(hub.subscriberCount(), 1u);

    std::string body = "client-sent garbage";
    controller.handleNewMessage(conn, std::move(body), drogon::WebSocketMessageType::Text);

    // No state change.
    EXPECT_EQ(hub.subscriberCount(), 1u);
    EXPECT_EQ(conn->sentCount(), 0u);
    EXPECT_FALSE(conn->isClosed());
}

TEST(UiStreamController, HandleNewConnectionWithoutInstallForceCloses) {
    // No install() — simulate a bootstrap defect.
    UiStreamController::uninstall();
    UiStreamController controller;
    auto conn = std::make_shared<FakeWebSocketConnection>();
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::HttpMethod::Get);

    controller.handleNewConnection(req, conn);

    EXPECT_TRUE(conn->isClosed());
}
