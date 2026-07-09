// extern "C" factory triplet for the DaviCal plugin .so. Kept in its
// own translation unit so the (testable) DaviCalAdapter class body
// can live inside aid_davical_internals — the test exe links that
// static library and can construct DaviCalAdapter directly without
// going through dlopen for every assertion.

#include <trantor/net/EventLoop.h>

#include <chrono>
#include <exception>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "aid/abi/PluginAbiTag.h"
#include "aid/abi/PluginContract.h"
#include "aid/adapters/davical/DaviCalAdapter.h"
#include "aid/adapters/support/HttpSupport.h"
#include "aid/crosscutting/Logger.h"
#include "aid/infrastructure/HttpClient.h"
#include "aid/ports/AddressBook.h"

#define AID_PLUGIN_EXPORT __attribute__((visibility("default")))

namespace aid::adapters::davical {
namespace {

// JSON section → DaviCalConfig. Rejects with nullopt on missing
// required keys so the factory can return
// nullptr and PluginLoader surfaces PluginAbiMismatch up to Main.
[[nodiscard]] std::optional<DaviCalConfig> parseConfig(std::string_view json) {
    using aid::crosscutting::Logger;
    using aid::crosscutting::LogType;
    try {
        auto root = nlohmann::json::parse(json);
        DaviCalConfig cfg;
        cfg.libPath = root.value("libPath", std::string{});
        cfg.bookAddresses = root.value("bookAddresses", std::string{});
        cfg.bookCompanies = root.value("bookCompanies", std::string{});
        cfg.user = root.value("user", std::string{});
        cfg.password = root.value("password", std::string{});
        cfg.defaultRegion = root.value("defaultRegion", std::string{});
        if (cfg.bookAddresses.empty() || cfg.bookCompanies.empty() || cfg.defaultRegion.empty()) {
            Logger::instance().error(
                "davical plugin: missing required keys (bookAddresses / bookCompanies / "
                "defaultRegion)",
                LogType::BACKEND);
            return std::nullopt;
        }
        return cfg;
    } catch (const std::exception& e) {
        // Password is in the JSON body but NEVER logged. The e.what()
        // from nlohmann on a parse error names the bad token, not the
        // whole body; safe to forward.
        Logger::instance().error(
            std::string{"davical plugin: config_json parse failed: "} + e.what(), LogType::BACKEND);
        return std::nullopt;
    }
}

// Best-effort HttpClient base URL extraction. The config gives full
// CardDAV URLs (e.g. "http://localhost/davical/caldav.php/aid/addresses/")
// and HttpClient needs only the host part — anything past the third '/'
// is the path the per-call REPORT will pass in. Returns empty on a
// missing scheme so the caller can fail the factory with a clear log
// line rather than constructing an HttpClient with garbage.
[[nodiscard]] std::string httpClientBaseFromBookUrl(std::string_view bookUrl) {
    // Shared scheme+host extraction (aid::adapters::support). DaviCal's
    // no-scheme policy: empty string, so the caller fails the factory with a
    // clear log rather than building an HttpClient with garbage.
    return aid::adapters::support::schemeAndHost(bookUrl).value_or(std::string{});
}

} // namespace
} // namespace aid::adapters::davical

extern "C" AID_PLUGIN_EXPORT aid::ports::AddressBook* create_AddressBook(const char* config_json,
                                                                         void* event_loop) {
    using aid::adapters::davical::DaviCalAdapter;
    using aid::adapters::davical::DaviCalConfig;
    using aid::adapters::davical::httpClientBaseFromBookUrl;
    using aid::adapters::davical::parseConfig;
    using aid::crosscutting::Logger;
    using aid::crosscutting::LogType;

    if (config_json == nullptr || event_loop == nullptr) {
        Logger::instance().error("davical plugin: factory got nullptr config_json/event_loop",
                                 LogType::BACKEND);
        return nullptr;
    }

    try {
        auto parsed = parseConfig(std::string{config_json});
        if (!parsed) {
            return nullptr;
        }

        // void* erasure point — keeps
        // trantor types off the extern "C" boundary.
        auto* loop = static_cast<trantor::EventLoop*>(event_loop);

        // 10 s read timeout (DaviCal is on the
        // shorter end of the upstream timeout spectrum).
        aid::infrastructure::UpstreamConfig httpCfg{};
        httpCfg.readTimeout = std::chrono::seconds{10};

        auto base = httpClientBaseFromBookUrl(parsed->bookAddresses);
        if (base.empty()) {
            Logger::instance().error("davical plugin: bookAddresses URL missing http(s):// scheme",
                                     LogType::BACKEND);
            return nullptr;
        }

        auto http = std::make_unique<aid::infrastructure::HttpClient>(base, httpCfg, *loop);
        return new DaviCalAdapter(std::move(http), std::move(*parsed));
    } catch (const std::bad_alloc&) {
        Logger::instance().error("davical plugin: factory OOM", LogType::BACKEND);
        return nullptr;
    } catch (const std::exception& e) {
        Logger::instance().error(std::string{"davical plugin: factory threw: "} + e.what(),
                                 LogType::BACKEND);
        return nullptr;
    } catch (...) {
        Logger::instance().error("davical plugin: factory threw unknown", LogType::BACKEND);
        return nullptr;
    }
}

extern "C" AID_PLUGIN_EXPORT void destroy_AddressBook(aid::ports::AddressBook* p) {
    delete p;
}

extern "C" AID_PLUGIN_EXPORT int aid_plugin_api_version(void) {
    return 1;
}

// ABI layout guard: the daemon compares this against its own
// aid::abi::kPluginAbiLayoutTag and refuses to load on mismatch, turning a
// silent heap-corruption SIGSEGV into a clean startup refusal.
extern "C" AID_PLUGIN_EXPORT std::uint64_t aid_plugin_abi_layout_tag(void) {
    return aid::abi::kPluginAbiLayoutTag;
}

// BF3 stale-plugin guard: the behaviour-contract tag this `.so` was built
// against. The daemon logs + compares it at startup, and scripts/deploy.sh
// greps it from the just-copied `.so`, so a stale plugin that lags the daemon
// fails loudly instead of silently running degraded.
extern "C" AID_PLUGIN_EXPORT const char* aid_plugin_contract_tag(void) {
    return aid::abi::kPluginContractTag;
}
