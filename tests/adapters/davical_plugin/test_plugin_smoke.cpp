// DaviCal plugin .so smoke test — dlopen the real aid_davical_plugin.so,
// confirm the extern "C" factory triplet resolves and the factory's
// catch-all error path engages on malformed JSON. Slice 5 adds a
// full lookup round-trip; slice 2's job is the ABI / link-order /
// symbol-visibility check.

#include <dlfcn.h>
#include <gtest/gtest.h>
#include <trantor/net/EventLoop.h>
#include <unistd.h>

#include <future>
#include <memory>
#include <string>
#include <thread>

#include "aid/infrastructure/PluginLoader.h"
#include "aid/ports/AddressBook.h"

#ifndef AID_DAVICAL_PLUGIN_PATH
#error "AID_DAVICAL_PLUGIN_PATH must be defined by CMake"
#endif

namespace {

// Same shape as the OpenProject smoke test — an EventLoop on its own
// thread, joined on destruction. The DaviCal factory takes a
// trantor::EventLoop* (cast through void*) for the in-process
// HttpClient it builds.
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

// Minimal config the factory needs for construction to succeed: full
// CardDAV URLs (just for the http://host derivation), user/password
// (logged-free), and defaultRegion. Slice 5's smoke test will swap the
// host for a local fake.
[[nodiscard]] std::string buildConfigJson() {
    return R"({
        "libPath": "ignored-at-test-time",
        "bookAddresses": "http://127.0.0.1:1/davical/caldav.php/aid/addresses/",
        "bookCompanies": "http://127.0.0.1:1/davical/caldav.php/aid/companies/",
        "user": "aid",
        "password": "test",
        "defaultRegion": "DE"
    })";
}

} // namespace

TEST(DaviCalPluginSmoke, FactoryTripletIsResolvableViaDlopen) {
    // Open the .so directly and confirm the three required symbols
    // resolve. Hidden visibility on the MODULE target keeps everything
    // else invisible; the three AID_PLUGIN_EXPORT-marked symbols must
    // be present. NEVER dlclose.
    void* handle = ::dlopen(AID_DAVICAL_PLUGIN_PATH, RTLD_NOW);
    ASSERT_NE(handle, nullptr) << ::dlerror();

    EXPECT_NE(::dlsym(handle, "create_AddressBook"), nullptr);
    EXPECT_NE(::dlsym(handle, "destroy_AddressBook"), nullptr);
    EXPECT_NE(::dlsym(handle, "aid_plugin_api_version"), nullptr);
    // BF3 stale-plugin guard symbol — every plugin built after the guard
    // exports it; the daemon refuses to start without it.
    EXPECT_NE(::dlsym(handle, "aid_plugin_contract_tag"), nullptr);
}

TEST(DaviCalPluginSmoke, PluginLoaderLoadsWithLoopAndApiVersionMatches) {
    LoopThread lt;
    const std::string cfg = buildConfigJson();

    aid::infrastructure::PluginLoader<aid::ports::AddressBook> loader;
    auto r = loader.loadWithLoop(AID_DAVICAL_PLUGIN_PATH, "create_AddressBook",
                                 "destroy_AddressBook", cfg, &lt.loop(), ::geteuid());
    ASSERT_TRUE(r.has_value()) << r.error().message;
    ASSERT_NE(loader.get(), nullptr);

    auto v = loader.apiVersion();
    ASSERT_TRUE(v.has_value());
    ASSERT_TRUE(v->has_value());
    EXPECT_EQ(**v, 1);
}

TEST(DaviCalPluginSmoke, FactoryWithMissingRequiredKeysReturnsNullptr) {
    // PluginLoader treats a nullptr factory result as PluginAbiMismatch.
    // Missing bookAddresses / bookCompanies / defaultRegion → reject
    // (the explicit branch inside parseConfig).
    LoopThread lt;
    const std::string missingKeysCfg = R"({"user":"aid","password":"x"})";

    aid::infrastructure::PluginLoader<aid::ports::AddressBook> loader;
    auto r = loader.loadWithLoop(AID_DAVICAL_PLUGIN_PATH, "create_AddressBook",
                                 "destroy_AddressBook", missingKeysCfg, &lt.loop(), ::geteuid());
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, aid::plumbing::ErrorCode::PluginAbiMismatch);
}

TEST(DaviCalPluginSmoke, FactoryWithSyntaxErrorJsonReturnsNullptr) {
    // Exercises the nlohmann::json::parse catch path (vs the
    // missing-required-keys branch above). Both must produce nullptr
    // and PluginAbiMismatch, never a throw across the .so boundary.
    LoopThread lt;
    const std::string syntaxErrorCfg = "{not json";

    aid::infrastructure::PluginLoader<aid::ports::AddressBook> loader;
    auto r = loader.loadWithLoop(AID_DAVICAL_PLUGIN_PATH, "create_AddressBook",
                                 "destroy_AddressBook", syntaxErrorCfg, &lt.loop(), ::geteuid());
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, aid::plumbing::ErrorCode::PluginAbiMismatch);
}
