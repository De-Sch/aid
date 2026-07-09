#pragma once

// DaviCalAdapter — concrete AddressBook implementation backed by CardDAV.
//
// The plugin .so exposes this class via the extern "C" factory triplet
// defined in DaviCalAdapter.cpp. Composition mirrors OpenProjectAdapter:
// DcHttp wraps the daemon's shared HttpClient (Basic auth + Depth: 1),
// DcVCardParser turns the multistatus body into Contact, and this
// facade orchestrates canonicalize → exact lookup → prefix lookup →
// nullopt.
//
// canonicalize() is the single authority on "is this a phone number?";
// delegated to libphonenumber.

#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "aid/adapters/davical/internal/DcHttp.h"
#include "aid/adapters/davical/internal/DcVCardParser.h"
#include "aid/infrastructure/HttpClient.h"
#include "aid/plumbing/Result.h"
#include "aid/plumbing/Task.h"
#include "aid/ports/AddressBook.h"
#include "aid/value-types/Contact.h"
#include "aid/value-types/Ids.h"

namespace aid::adapters::davical {

// Slice-2 placement: DaviCal config travels here as a local struct so
// the plugin compiles without touching crosscutting/Config.h. Slice 5
// promotes this to aid::crosscutting::AddressSystem (alias DaviCal)
// once the daemon's bootstrap also needs to read the JSON section.
struct DaviCalConfig {
    std::string libPath;
    std::string bookAddresses;
    std::string bookCompanies;
    std::string user;
    std::string password;
    std::string defaultRegion;
};

class DaviCalAdapter final : public aid::ports::AddressBook {
public:
    DaviCalAdapter(std::unique_ptr<aid::infrastructure::HttpClient> http, DaviCalConfig cfg);

    ~DaviCalAdapter() override = default;

    DaviCalAdapter(const DaviCalAdapter&) = delete;
    DaviCalAdapter& operator=(const DaviCalAdapter&) = delete;
    DaviCalAdapter(DaviCalAdapter&&) = delete;
    DaviCalAdapter& operator=(DaviCalAdapter&&) = delete;

    // ─── AddressBook port ───────────────────────────────────────────────

    // Shutdown hook: forwards to the shared HttpClient's
    // cancellation so a worker suspended in a DaviCal lookup unwinds promptly
    // during the graceful drain.
    void cancelPendingRequests() noexcept override;

    [[nodiscard]] aid::PhoneNumber canonicalize(aid::PhoneNumber raw) const noexcept override;

    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<std::optional<aid::Contact>>>
    lookup(aid::PhoneNumber number) override;

    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<void>> ping() override;

private:
    // Construction order: http_ first (DcHttp borrows it by reference).
    std::unique_ptr<aid::infrastructure::HttpClient> httpClient_;
    DaviCalConfig cfg_;
    internal::DcHttp http_;
    // DcVCardParser is stateless (static methods) — no member needed.
};

} // namespace aid::adapters::davical
