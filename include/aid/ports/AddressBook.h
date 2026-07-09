#pragma once

#include <optional>

#include "aid/plumbing/Result.h"
#include "aid/plumbing/Task.h"
#include "aid/value-types/Contact.h"
#include "aid/value-types/Ids.h"

// Abstract port for the address-book backend. `canonicalize` is the single
// enforcement point for the canonical-E.164 invariant on PhoneNumber.

namespace aid::ports {

class AddressBook {
public:
    virtual ~AddressBook() = default;

    AddressBook() = default;
    AddressBook(const AddressBook&) = delete;
    AddressBook& operator=(const AddressBook&) = delete;
    AddressBook(AddressBook&&) = delete;
    AddressBook& operator=(AddressBook&&) = delete;

    // Shutdown hook — see TicketStore::cancelPendingRequests.
    // Best-effort cancellation of in-flight upstream (address-system) requests so
    // a worker suspended in a contact lookup unwinds promptly during the
    // graceful drain, before this port is destroyed. Default: no-op.
    virtual void cancelPendingRequests() noexcept {}

    [[nodiscard]] virtual PhoneNumber canonicalize(PhoneNumber raw) const noexcept = 0;

    [[nodiscard]] virtual plumbing::Task<plumbing::Result<std::optional<Contact>>>
    lookup(PhoneNumber number) = 0;

    // Cheap reachability probe for HealthService::bootstrapPing.
    // the address-book adapter's implementation hits a cheap GET on the configured base URL;
    // any 2xx → ok, anything else → Error{UpstreamUnavailable, ...}.
    // Implementations must not throw across the plugin ABI.
    [[nodiscard]] virtual plumbing::Task<plumbing::Result<void>> ping() = 0;
};

} // namespace aid::ports
