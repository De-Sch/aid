#pragma once

#include <trantor/net/EventLoop.h>

#include <chrono>
#include <filesystem>
#include <future>
#include <mutex>
#include <thread>

#include "aid/crosscutting/Logger.h"

// Shared harness for the integration tests. Pre-existing files
// it_call_dispatch.cpp / it_incoming_call.cpp keep their own copies of
// LoopThread + LoggerOnce; new files include this header instead.

namespace aid::tests::integration {

class LoopThread {
public:
    LoopThread() {
        std::promise<trantor::EventLoop*> ready;
        auto fut = ready.get_future();
        thread_ = std::thread([&ready] {
            trantor::EventLoop loop;
            ready.set_value(&loop);
            loop.loop();
        });
        loop_ = fut.get();
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

inline std::once_flag g_loggerInitFlag;

struct LoggerOnce {
    LoggerOnce() {
        std::call_once(g_loggerInitFlag, [] {
            const auto tmp = std::filesystem::temp_directory_path();
            aid::crosscutting::Logger::initialize(aid::crosscutting::LogLevel::ERROR,
                                                  (tmp / "aid_integration_backend.log").string(),
                                                  (tmp / "aid_integration_frontend.log").string());
        });
    }
};

template <class Pred>
[[nodiscard]] inline bool
waitUntil(Pred&& pred, std::chrono::milliseconds budget = std::chrono::milliseconds{2000}) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return false;
}

} // namespace aid::tests::integration
