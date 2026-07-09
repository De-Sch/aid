#pragma once

#include <chrono>
#include <deque>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aid/adapters/openproject/internal/HttpDispatcher.h"
#include "aid/infrastructure/HttpClient.h"
#include "aid/plumbing/Error.h"
#include "aid/plumbing/Result.h"
#include "aid/plumbing/Task.h"

// Test-only fakes for OpHttp / OpUserRepo / OpTicketRepo.
//
// FakeHttpDispatcher: records every send() call and replies with the
// next scripted HttpResponse / Error. Tests treat the resulting Task<>
// as synchronous because the fake's send() never co_awaits anything —
// it just runs straight to co_return (initial_suspend=suspend_never).
//
// FakeSleeper: returns an instantly-ready Task<void> while recording
// the requested duration, so OpHttp::retryOn409's 50/100/200/400/800 ms
// ladder can be asserted exactly without burning ~1.5 s of wall time
// per test.

namespace aid::test_support {

struct RecordedCall {
    std::string method;
    std::string path;
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;
};

class FakeHttpDispatcher final : public aid::adapters::openproject::HttpDispatcher {
public:
    using SendResult = aid::plumbing::Result<aid::infrastructure::HttpResponse>;

    [[nodiscard]] aid::plumbing::Task<SendResult>
    send(std::string_view method, std::string_view path, std::string_view body,
         const aid::infrastructure::Headers& hdrs) override {
        RecordedCall rec;
        rec.method.assign(method);
        rec.path.assign(path);
        rec.body.assign(body);
        for (const auto& kv : hdrs.kv)
            rec.headers.emplace_back(kv.first, kv.second);
        calls_.push_back(std::move(rec));

        if (scripted_.empty()) {
            co_return aid::plumbing::unexpected{aid::plumbing::Error{
                aid::plumbing::ErrorCode::Unknown,
                "FakeHttpDispatcher: ran out of scripted responses (test wired wrong)",
                std::nullopt}};
        }
        auto next = std::move(scripted_.front());
        scripted_.pop_front();
        co_return next;
    }

    void enqueueResponse(int status, std::string body) {
        aid::infrastructure::HttpResponse r;
        r.status = status;
        r.body = std::move(body);
        scripted_.emplace_back(std::move(r));
    }

    void enqueueError(aid::plumbing::ErrorCode code, std::string message) {
        scripted_.emplace_back(aid::plumbing::unexpected{
            aid::plumbing::Error{code, std::move(message), std::nullopt}});
    }

    [[nodiscard]] const std::vector<RecordedCall>& calls() const noexcept { return calls_; }

private:
    std::deque<SendResult> scripted_;
    std::vector<RecordedCall> calls_;
};

class FakeSleeper {
public:
    [[nodiscard]] aid::adapters::openproject::Sleeper sleeper() {
        return [this](std::chrono::milliseconds d) -> aid::plumbing::Task<void> {
            durations.push_back(d);
            co_return;
        };
    }

    std::vector<std::chrono::milliseconds> durations;
};

} // namespace aid::test_support
