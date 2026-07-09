#include "FakeAddressBook.h"

#include <utility>

namespace aid::fakes {

aid::PhoneNumber FakeAddressBook::canonicalize(aid::PhoneNumber raw) const noexcept {
    // canonicalize is noexcept by spec; swallow any allocator-induced failure
    // and return empty so callers route to incognito rather than crashing.
    try {
        canonicalizeCalls.push_back(raw);
        auto it = canonicalizeMap.find(raw.v);
        if (it != canonicalizeMap.end()) {
            return aid::PhoneNumber{it->second};
        }
        if (defaultEmpty) {
            return aid::PhoneNumber{};
        }
        return raw;
    } catch (...) {
        return aid::PhoneNumber{};
    }
}

aid::plumbing::Task<aid::plumbing::Result<std::optional<aid::Contact>>>
FakeAddressBook::lookup(aid::PhoneNumber number) {
    lookupCalls.push_back(number);
    if (nextLookup.empty()) {
        co_return aid::plumbing::unexpected{
            aid::plumbing::Error{aid::plumbing::ErrorCode::InvariantViolation,
                                 "FakeAddressBook: no canned response for lookup", std::nullopt}};
    }
    auto v = std::move(nextLookup.front());
    nextLookup.pop_front();
    co_return v;
}

aid::plumbing::Task<aid::plumbing::Result<void>> FakeAddressBook::ping() {
    ++ping_calls;
    if (nextPing.empty()) {
        co_return aid::plumbing::unexpected{
            aid::plumbing::Error{aid::plumbing::ErrorCode::InvariantViolation,
                                 "FakeAddressBook: no canned response for ping", std::nullopt}};
    }
    auto v = std::move(nextPing.front());
    nextPing.pop_front();
    co_return v;
}

} // namespace aid::fakes
