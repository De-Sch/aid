#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include "aid/abi/PluginAbiTag.h"
#include "aid/abi/PluginContract.h"
#include "aid/infrastructure/PluginLoader.h"
#include "aid/plumbing/Error.h"
#include "test_plugin/test_port.h"

namespace {

using aid::infrastructure::checkPluginAbiLayoutTag;
using aid::infrastructure::checkPluginApiVersion;
using aid::infrastructure::checkPluginContractTag;
using aid::infrastructure::PluginLoader;
using aid::plumbing::Error;
using aid::plumbing::ErrorCode;

#ifndef AID_TEST_PLUGIN_PATH
#error "AID_TEST_PLUGIN_PATH must be defined by the test target"
#endif

const std::string kPluginPath = AID_TEST_PLUGIN_PATH;

class PluginLoaderTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto pid = static_cast<std::uint64_t>(::getpid());
        static std::atomic<std::uint64_t> counter{0};
        const auto n = counter.fetch_add(1, std::memory_order_relaxed);
        dir_ = std::filesystem::temp_directory_path() /
               ("aid_loader_test_" + std::to_string(pid) + "_" + std::to_string(n));
        std::filesystem::create_directories(dir_);
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    [[nodiscard]] std::string copyPluginTo(const std::string& filename,
                                           ::mode_t mode = 0640) const {
        const auto dst = (dir_ / filename).string();
        std::ifstream in(kPluginPath, std::ios::binary);
        std::ofstream out(dst, std::ios::binary | std::ios::trunc);
        out << in.rdbuf();
        out.close();
        if (::chmod(dst.c_str(), mode) != 0) {
            ADD_FAILURE() << "chmod " << dst << ": " << std::strerror(errno);
        }
        return dst;
    }

    std::filesystem::path dir_;
};

TEST_F(PluginLoaderTest, LoadsHappyPathAndCallsFactory) {
    PluginLoader<TestPort> loader;
    const auto r = loader.load(kPluginPath, "create_TestPort", "destroy_TestPort",
                               R"({"hello":"world"})", ::geteuid());
    ASSERT_TRUE(r.has_value()) << r.error().message;

    auto* port = loader.get();
    ASSERT_NE(port, nullptr);
    EXPECT_EQ(port->answer(), 42);
    EXPECT_EQ(port->echoConfig(), R"({"hello":"world"})");
}

TEST_F(PluginLoaderTest, ApiVersionReturnsExportedNumber) {
    PluginLoader<TestPort> loader;
    ASSERT_TRUE(loader.load(kPluginPath, "create_TestPort", "destroy_TestPort", "{}", ::geteuid())
                    .has_value());

    const auto v = loader.apiVersion();
    ASSERT_TRUE(v.has_value());
    ASSERT_TRUE(v->has_value()) << "aid_plugin_api_version symbol missing";
    EXPECT_EQ(**v, 1);
}

TEST_F(PluginLoaderTest, ApiVersionBeforeLoadReturnsPluginAbiMismatch) {
    PluginLoader<TestPort> loader;
    const auto v = loader.apiVersion();
    ASSERT_FALSE(v.has_value());
    EXPECT_EQ(v.error().code, ErrorCode::PluginAbiMismatch);
}

// --- checkPluginApiVersion (handshake helper) --------------------------

TEST(CheckPluginApiVersion, MatchingVersionIsOk) {
    const auto r = checkPluginApiVersion(std::optional<int>{1}, "TestPort");
    EXPECT_TRUE(r.has_value()) << (r ? "" : r.error().message);
}

TEST(CheckPluginApiVersion, MismatchedVersionIsAbiMismatch) {
    const auto r = checkPluginApiVersion(std::optional<int>{2}, "TestPort");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ErrorCode::PluginAbiMismatch);
    // Message names the plugin and both versions for the operator.
    EXPECT_NE(r.error().message.find("TestPort"), std::string::npos);
    EXPECT_NE(r.error().message.find('2'), std::string::npos);
}

TEST(CheckPluginApiVersion, AbsentSymbolIsAllowed) {
    // Empty optional == plugin omitted aid_plugin_api_version(); the handshake
    // is optional-but-recommended, so this must NOT block startup.
    const auto r = checkPluginApiVersion(std::optional<int>{}, "TestPort");
    EXPECT_TRUE(r.has_value()) << (r ? "" : r.error().message);
}

TEST(CheckPluginApiVersion, OuterErrorIsPropagated) {
    aid::plumbing::Result<std::optional<int>> in =
        aid::plumbing::unexpected{Error{ErrorCode::PluginAbiMismatch, "before load", std::nullopt}};
    const auto r = checkPluginApiVersion(in, "TestPort");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ErrorCode::PluginAbiMismatch);
    EXPECT_EQ(r.error().message, "before load");
}

// --- ABI layout tag (guard) -------------------------------------------

TEST(PluginAbiLayoutTag, IsNonZeroAndStable) {
    // A degenerate all-zero fingerprint would silently accept any plugin.
    EXPECT_NE(aid::abi::kPluginAbiLayoutTag, 0ULL);
    // constexpr — every evaluation in this build yields the same value.
    EXPECT_EQ(aid::abi::kPluginAbiLayoutTag, aid::abi::kPluginAbiLayoutTag);
}

TEST_F(PluginLoaderTest, AbiLayoutTagReturnsExportedValue) {
    PluginLoader<TestPort> loader;
    ASSERT_TRUE(loader.load(kPluginPath, "create_TestPort", "destroy_TestPort", "{}", ::geteuid())
                    .has_value());

    const auto t = loader.abiLayoutTag();
    ASSERT_TRUE(t.has_value());
    ASSERT_TRUE(t->has_value()) << "aid_plugin_abi_layout_tag symbol missing";
    // The .so was built from the same headers as this test exe, so it matches.
    EXPECT_EQ(**t, aid::abi::kPluginAbiLayoutTag);
}

TEST_F(PluginLoaderTest, AbiLayoutTagBeforeLoadReturnsPluginAbiMismatch) {
    PluginLoader<TestPort> loader;
    const auto t = loader.abiLayoutTag();
    ASSERT_FALSE(t.has_value());
    EXPECT_EQ(t.error().code, ErrorCode::PluginAbiMismatch);
}

// --- checkPluginAbiLayoutTag (guard helper) ---------------------------

TEST(CheckPluginAbiLayoutTag, MatchingTagIsOk) {
    const auto r = checkPluginAbiLayoutTag(std::optional<std::uint64_t>{0xABCDEF01ULL}, "TestPort",
                                           0xABCDEF01ULL);
    EXPECT_TRUE(r.has_value()) << (r ? "" : r.error().message);
}

TEST(CheckPluginAbiLayoutTag, MismatchedTagIsAbiMismatch) {
    const auto r =
        checkPluginAbiLayoutTag(std::optional<std::uint64_t>{0x1ULL}, "TestPort", 0x2ULL);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ErrorCode::PluginAbiMismatch);
    // Message names the plugin and renders both tags in hex for the operator.
    EXPECT_NE(r.error().message.find("TestPort"), std::string::npos);
    EXPECT_NE(r.error().message.find("0x0000000000000001"), std::string::npos);
    EXPECT_NE(r.error().message.find("0x0000000000000002"), std::string::npos);
}

TEST(CheckPluginAbiLayoutTag, AbsentSymbolIsRefused) {
    // Unlike the version handshake, a MISSING layout tag is a hard failure:
    // both shipped plugins always export it, so absence means a stale build.
    const auto r = checkPluginAbiLayoutTag(std::optional<std::uint64_t>{}, "TestPort");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ErrorCode::PluginAbiMismatch);
    EXPECT_NE(r.error().message.find("TestPort"), std::string::npos);
}

TEST(CheckPluginAbiLayoutTag, OuterErrorIsPropagated) {
    aid::plumbing::Result<std::optional<std::uint64_t>> in =
        aid::plumbing::unexpected{Error{ErrorCode::PluginAbiMismatch, "before load", std::nullopt}};
    const auto r = checkPluginAbiLayoutTag(in, "TestPort");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ErrorCode::PluginAbiMismatch);
    EXPECT_EQ(r.error().message, "before load");
}

// --- behaviour-contract tag (BF3 stale-plugin guard) ----------------------

TEST_F(PluginLoaderTest, ContractTagReturnsExportedValue) {
    PluginLoader<TestPort> loader;
    ASSERT_TRUE(loader.load(kPluginPath, "create_TestPort", "destroy_TestPort", "{}", ::geteuid())
                    .has_value());

    const auto t = loader.contractTag();
    ASSERT_TRUE(t.has_value());
    ASSERT_TRUE(t->has_value()) << "aid_plugin_contract_tag symbol missing";
    // The .so was built from the same header as this test exe, so it matches.
    EXPECT_EQ(**t, std::string{aid::abi::kPluginContractTag});
}

TEST_F(PluginLoaderTest, ContractTagBeforeLoadReturnsPluginAbiMismatch) {
    PluginLoader<TestPort> loader;
    const auto t = loader.contractTag();
    ASSERT_FALSE(t.has_value());
    EXPECT_EQ(t.error().code, ErrorCode::PluginAbiMismatch);
}

TEST(CheckPluginContractTag, MatchingTagIsOk) {
    const auto r = checkPluginContractTag(std::optional<std::string>{"AID_PLUGIN_CONTRACT=2"},
                                          "TestPort", "AID_PLUGIN_CONTRACT=2");
    EXPECT_TRUE(r.has_value()) << (r ? "" : r.error().message);
}

TEST(CheckPluginContractTag, MismatchedTagIsAbiMismatch) {
    const auto r = checkPluginContractTag(std::optional<std::string>{"AID_PLUGIN_CONTRACT=1"},
                                          "TestPort", "AID_PLUGIN_CONTRACT=2");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ErrorCode::PluginAbiMismatch);
    // Message names the plugin and both tags so the operator can act on it.
    EXPECT_NE(r.error().message.find("TestPort"), std::string::npos);
    EXPECT_NE(r.error().message.find("AID_PLUGIN_CONTRACT=1"), std::string::npos);
    EXPECT_NE(r.error().message.find("AID_PLUGIN_CONTRACT=2"), std::string::npos);
}

TEST(CheckPluginContractTag, AbsentSymbolIsRefused) {
    // Like the layout guard, a MISSING contract tag is a hard failure: every
    // plugin shipped after the guard exports it, so absence means a stale .so.
    const auto r = checkPluginContractTag(std::optional<std::string>{}, "TestPort");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ErrorCode::PluginAbiMismatch);
    EXPECT_NE(r.error().message.find("TestPort"), std::string::npos);
}

TEST(CheckPluginContractTag, OuterErrorIsPropagated) {
    aid::plumbing::Result<std::optional<std::string>> in =
        aid::plumbing::unexpected{Error{ErrorCode::PluginAbiMismatch, "before load", std::nullopt}};
    const auto r = checkPluginContractTag(in, "TestPort");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ErrorCode::PluginAbiMismatch);
    EXPECT_EQ(r.error().message, "before load");
}

TEST_F(PluginLoaderTest, MissingFactorySymbolReturnsPluginAbiMismatch) {
    PluginLoader<TestPort> loader;
    const auto r =
        loader.load(kPluginPath, "create_DoesNotExist", "destroy_TestPort", "{}", ::geteuid());
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ErrorCode::PluginAbiMismatch);
    EXPECT_NE(r.error().message.find("create_DoesNotExist"), std::string::npos);
}

TEST_F(PluginLoaderTest, MissingDestroySymbolReturnsPluginAbiMismatch) {
    PluginLoader<TestPort> loader;
    const auto r =
        loader.load(kPluginPath, "create_TestPort", "destroy_DoesNotExist", "{}", ::geteuid());
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ErrorCode::PluginAbiMismatch);
    EXPECT_NE(r.error().message.find("destroy_DoesNotExist"), std::string::npos);
}

TEST_F(PluginLoaderTest, DlopenFailsOnNonexistentPath) {
    const auto bogus = (dir_ / "does_not_exist.so").string();
    PluginLoader<TestPort> loader;
    const auto r = loader.load(bogus, "create_TestPort", "destroy_TestPort", "{}", ::geteuid());
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ErrorCode::PluginAbiMismatch);
}

TEST_F(PluginLoaderTest, WorldWritableRefused) {
    const auto copy = copyPluginTo("ww.so", 0666);
    PluginLoader<TestPort> loader;
    const auto r = loader.load(copy, "create_TestPort", "destroy_TestPort", "{}", ::geteuid());
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ErrorCode::PluginAbiMismatch);
    EXPECT_NE(r.error().message.find("world-writable"), std::string::npos);
}

TEST_F(PluginLoaderTest, WrongOwnerRefused) {
    const auto copy = copyPluginTo("owner.so", 0640);
    PluginLoader<TestPort> loader;
    const ::uid_t wrong = ::geteuid() + 1; // not equal to the file's owner
    const auto r = loader.load(copy, "create_TestPort", "destroy_TestPort", "{}", wrong);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ErrorCode::PluginAbiMismatch);
    EXPECT_NE(r.error().message.find("owner uid"), std::string::npos);
}

TEST_F(PluginLoaderTest, DestructorRunsDestroyerCleanly) {
    // Smoke test: destructor must call destroy_TestPort without crashing.
    // ASan would catch a double-delete or use-after-free in the deleter.
    {
        PluginLoader<TestPort> loader;
        ASSERT_TRUE(
            loader.load(kPluginPath, "create_TestPort", "destroy_TestPort", "{}", ::geteuid())
                .has_value());
        EXPECT_NE(loader.get(), nullptr);
    }
    SUCCEED();
}

} // namespace
