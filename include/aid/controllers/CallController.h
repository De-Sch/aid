#pragma once

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include <functional>
#include <optional>

#include "aid/value-types/CallEvent.h"

namespace aid::infrastructure {
class Mailbox;
class Wal;
} // namespace aid::infrastructure

namespace aid::crosscutting {
class Logger;
class CorrelationId;
} // namespace aid::crosscutting

namespace aid::controllers {

// POST /call dispatcher. Pure plumbing: parse → fsync WAL → enqueue → 202.
// The enqueue happens *before* 202 so we can respond
// 503 on backpressure rejection instead of lying. Body lifetime:
// copy req->getBody() into a std::string before WAL append, since the
// framework's underlying buffer is released after the handler returns.
class CallController {
public:
    CallController(aid::infrastructure::Wal& wal, aid::infrastructure::Mailbox& mailbox,
                   aid::crosscutting::Logger& logger, aid::crosscutting::CorrelationId& cid);

    CallController(const CallController&) = delete;
    CallController& operator=(const CallController&) = delete;
    CallController(CallController&&) = delete;
    CallController& operator=(CallController&&) = delete;
    ~CallController() = default;

    void handlePost(const drogon::HttpRequestPtr& req,
                    std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    // Five-way switch on j["event"]. Honours the wire quirks of the
    // upstream call handler: Outgoing has no `dialed`, Accepted's
    // `user` is optional, Transfer uses `newuser` (not `user`).
    [[nodiscard]] static std::optional<aid::CallEvent> decodeJson(std::string_view body);

private:
    aid::infrastructure::Wal& wal_;
    aid::infrastructure::Mailbox& mailbox_;
    aid::crosscutting::Logger& logger_;
    aid::crosscutting::CorrelationId& cid_;
};

} // namespace aid::controllers
