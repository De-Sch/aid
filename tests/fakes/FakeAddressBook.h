#pragma once

#include <deque>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "aid/plumbing/Error.h"
#include "aid/plumbing/Result.h"
#include "aid/plumbing/Task.h"
#include "aid/ports/AddressBook.h"
#include "aid/value-types/Contact.h"
#include "aid/value-types/Ids.h"

namespace aid::fakes {

// FakeAddressBook lets tests script canonicalize() (the synchronous,
// noexcept discriminator hinges on) without dragging in libphonenumber.
// canonicalizeMap maps raw input → output; missing entries return raw
// unchanged unless defaultEmpty is true (used to exercise the incognito
// branch). lookup() pops canned responses.
class FakeAddressBook final : public aid::ports::AddressBook {
public:
    std::unordered_map<std::string, std::string> canonicalizeMap;
    bool defaultEmpty = false;

    mutable std::vector<aid::PhoneNumber> canonicalizeCalls;
    std::vector<aid::PhoneNumber> lookupCalls;

    std::deque<aid::plumbing::Result<std::optional<aid::Contact>>> nextLookup;
    std::deque<aid::plumbing::Result<void>> nextPing;

    int ping_calls = 0;

    [[nodiscard]] aid::PhoneNumber canonicalize(aid::PhoneNumber raw) const noexcept override;

    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<std::optional<aid::Contact>>>
    lookup(aid::PhoneNumber number) override;

    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<void>> ping() override;
};

} // namespace aid::fakes
