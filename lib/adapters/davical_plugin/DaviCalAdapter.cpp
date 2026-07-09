// DaviCalAdapter class body — composed of canonicalize() (libphonenumber)
// and the two-step lookup() pipeline (DcHttp + DcVCardParser). The
// extern "C" factory triplet that the daemon dlopens lives in
// factory.cpp; keeping the class body in this .cpp (which is part of
// aid_davical_internals) lets tests construct DaviCalAdapter directly.

#include "aid/adapters/davical/DaviCalAdapter.h"

#include <phonenumbers/phonenumber.pb.h>
#include <phonenumbers/phonenumberutil.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aid/adapters/davical/internal/DcVCardParser.h"
#include "aid/infrastructure/HttpClient.h"
#include "aid/plumbing/Error.h"
#include "aid/plumbing/Result.h"
#include "aid/plumbing/Task.h"
#include "aid/value-types/Contact.h"
#include "aid/value-types/Ids.h"

namespace aid::adapters::davical {

DaviCalAdapter::DaviCalAdapter(std::unique_ptr<aid::infrastructure::HttpClient> http,
                               DaviCalConfig cfg)
    : httpClient_(std::move(http)), cfg_(std::move(cfg)),
      http_(*httpClient_, cfg_.user, cfg_.password) {
}

void DaviCalAdapter::cancelPendingRequests() noexcept {
    // Abort any in-flight DaviCal request so a worker
    // suspended in a contact lookup unwinds at once. main() calls this during
    // the graceful drain, while this adapter is alive, then waits for the
    // mailboxes to go quiescent before releasing the plugin. httpClient_ is
    // non-null for the adapter's lifetime (set in the ctor init list).
    httpClient_->cancelInFlight();
}

namespace {

// EXTENSION_LENGTH: when the exact-match
// pass on bookAddresses misses, trim this many trailing digits off the
// canonical E.164 and prefix-search bookCompanies. 5 is the typical
// internal extension length on the systems we serve; pulling it into
// a named constexpr instead of the magic literal 5 at
// microkernel:DaviCal.cpp:338.
constexpr std::size_t EXTENSION_LENGTH = 5;

// E.164 contract: leading '+' followed by digits only. The XML builders
// below gate on this in every build mode; a future change that lets, say,
// a space or '<' through is rejected with InvalidInput rather than reaching
// the CardDAV REPORT body as an XML injection.
[[nodiscard]] bool isCanonicalE164(std::string_view s) noexcept {
    if (s.empty()) {
        return true; // empty is the explicit "incognito" signal, also safe to interpolate
    }
    if (s.front() != '+') {
        return false;
    }
    return std::all_of(s.begin() + 1, s.end(),
                       [](unsigned char c) { return std::isdigit(c) != 0; });
}

// CardDAV addressbook-query (RFC 6352 §8.6.4) for the exact-TEL pass.
// The number is interpolated verbatim into the XML body, so we hard-guard
// that it is canonical E.164 — only '+' and digits — in ALL build modes.
// Under NDEBUG the old assert() was a no-op; rejecting here keeps an XML
// injection out of the CardDAV REPORT body even if a future caller forgets
// to canonicalise. Returns InvalidInput on violation.
//
// match-type is "contains", not "equals": DaviCal applies the text-match
// against the RAW stored TEL value before our parser sees it, so an entry
// stored with stray surrounding whitespace (e.g. " +49304321", a common
// hand-entry mistake) would be filtered out server-side by an "equals"
// match and never reach us. "contains" lets the whitespace-padded value
// through; lookup() then enforces an exact match client-side on the
// parser-trimmed number. The needle is still canonical E.164, so the only
// false positives "contains" can add are strict superstrings of the full
// number — those are dropped by the client-side exact check.
[[nodiscard]] aid::plumbing::Result<std::string> buildExactQuery(aid::PhoneNumber number) {
    if (!isCanonicalE164(number.v)) {
        return aid::plumbing::unexpected{aid::plumbing::Error{
            aid::plumbing::ErrorCode::InvalidInput,
            "buildExactQuery: non-canonical E.164 input rejected (XML not escaped)", std::nullopt}};
    }
    return std::string{R"(<?xml version="1.0" encoding="utf-8" ?>
<C:addressbook-query xmlns:D="DAV:" xmlns:C="urn:ietf:params:xml:ns:carddav">
  <D:prop><C:address-data/></D:prop>
  <C:filter>
    <C:prop-filter name="TEL">
      <C:text-match match-type="contains">)"} +
           number.v + R"(</C:text-match>
    </C:prop-filter>
  </C:filter>
</C:addressbook-query>)";
}

// Prefix-match variant. The PhoneNumber here is already the trimmed
// head (canonical minus EXTENSION_LENGTH trailing digits), so a TEL
// containing that head is a candidate company main line. Same
// unconditional E.164 guard as buildExactQuery — see the rationale there.
//
// match-type is "contains" for the same whitespace reason: a company
// stored " +49304321" must still match the trimmed head "+4930", which a
// server-side "starts-with" would reject (the stored value starts with a
// space). The trimmed head is short, but "+" only appears at the head of
// an E.164 number, so "contains +49…" behaves like "starts-with" for
// clean data while tolerating padding; pickLongestCommonPrefix then
// narrows the multi-hit set client-side on the parser-trimmed numbers.
[[nodiscard]] aid::plumbing::Result<std::string> buildPrefixQuery(aid::PhoneNumber prefix) {
    if (!isCanonicalE164(prefix.v)) {
        return aid::plumbing::unexpected{aid::plumbing::Error{
            aid::plumbing::ErrorCode::InvalidInput,
            "buildPrefixQuery: non-canonical E.164 input rejected (XML not escaped)",
            std::nullopt}};
    }
    return std::string{R"(<?xml version="1.0" encoding="utf-8" ?>
<C:addressbook-query xmlns:D="DAV:" xmlns:C="urn:ietf:params:xml:ns:carddav">
  <D:prop><C:address-data/></D:prop>
  <C:filter>
    <C:prop-filter name="TEL">
      <C:text-match match-type="contains">)"} +
           prefix.v + R"(</C:text-match>
    </C:prop-filter>
  </C:filter>
</C:addressbook-query>)";
}

// Exact-TEL picker for the addresses (Person) pass. The "contains" server
// filter can return superstring false positives, and DcVCardParser already
// trims surrounding whitespace off every parsed TEL — so the authoritative
// exact match is "some parsed TEL equals the canonical number". First hit
// wins on ties (deterministic), mirroring pickLongestCommonPrefix.
[[nodiscard]] std::optional<aid::Contact> pickExactMatch(std::span<const aid::Contact> hits,
                                                         aid::PhoneNumber number) {
    for (const auto& c : hits) {
        for (const auto& tel : c.phoneNumbers) {
            if (tel.v == number.v) {
                return c;
            }
        }
    }
    return std::nullopt;
}

// CardDAV may return multiple Contacts when several entries match the
// filter (e.g. two TELs starting with the same prefix). Pick the one
// whose stored TEL has the longest character-by-character common
// prefix with the canonical incoming number — that's the closest
// match. Ties take the first occurrence; the spec calls this out only
// as "longest common prefix", not a tie-breaker requiring sort
// stability beyond "deterministic".
[[nodiscard]] std::optional<aid::Contact>
pickLongestCommonPrefix(std::span<const aid::Contact> hits, aid::PhoneNumber against) {
    if (hits.empty()) {
        return std::nullopt;
    }
    if (hits.size() == 1) {
        return hits[0];
    }
    auto lcp = [](std::string_view a, std::string_view b) noexcept {
        std::size_t i = 0;
        while (i < a.size() && i < b.size() && a[i] == b[i]) {
            ++i;
        }
        return i;
    };
    const aid::Contact* best = &hits[0];
    std::size_t bestLen = 0;
    for (const auto& c : hits) {
        for (const auto& tel : c.phoneNumbers) {
            const auto len = lcp(tel.v, against.v);
            if (len > bestLen) {
                bestLen = len;
                best = &c;
            }
        }
    }
    return *best;
}

} // namespace

aid::PhoneNumber DaviCalAdapter::canonicalize(aid::PhoneNumber raw) const noexcept {
    // The single authority on "is this a phone number?".
    // libphonenumber-cpp's PhoneNumberUtil::GetInstance() returns a
    // process-wide static singleton; thread-safe; cheap.
    //
    // belt-and-suspenders try/catch: libphonenumber-cpp does not
    // document noexcept on Parse/Format, and our port signature is
    // noexcept. Any failure (parse error, bad region, unrecognised
    // input, internal exception) collapses to PhoneNumber{} so the
    // incognito branch takes over upstream.
    try {
        auto* utilPtr = i18n::phonenumbers::PhoneNumberUtil::GetInstance();
        if (utilPtr == nullptr) {
            return aid::PhoneNumber{};
        }
        i18n::phonenumbers::PhoneNumber parsed;
        const auto status = utilPtr->Parse(raw.v, cfg_.defaultRegion, &parsed);
        if (status != i18n::phonenumbers::PhoneNumberUtil::NO_PARSING_ERROR) {
            return aid::PhoneNumber{};
        }
        if (!utilPtr->IsPossibleNumber(parsed)) {
            return aid::PhoneNumber{};
        }
        std::string e164;
        utilPtr->Format(parsed, i18n::phonenumbers::PhoneNumberUtil::E164, &e164);
        return aid::PhoneNumber{std::move(e164)};
    } catch (...) {
        return aid::PhoneNumber{};
    }
}

aid::plumbing::Task<aid::plumbing::Result<std::optional<aid::Contact>>>
DaviCalAdapter::lookup(aid::PhoneNumber number) {
    // Precondition: caller has already canonicalised
    // and branched on empty. We defend against accidental misuse by
    // returning nullopt for an empty number rather than firing a
    // useless REPORT at the address book.
    if (number.empty()) {
        co_return std::optional<aid::Contact>{};
    }

    // Step 1: exact match on the addresses book (Person). The "contains"
    // server filter tolerates whitespace-padded stored TELs; pickExactMatch
    // re-imposes exactness on the parser-trimmed numbers client-side.
    {
        auto query = buildExactQuery(number);
        if (!query) {
            co_return aid::plumbing::unexpected{query.error()};
        }
        auto resp = co_await http_.report(cfg_.bookAddresses, *query);
        if (!resp) {
            co_return aid::plumbing::unexpected{resp.error()};
        }
        auto contacts = internal::DcVCardParser::parse(*resp);
        if (auto picked = pickExactMatch(contacts, number)) {
            picked->kind = aid::AddressKind::Person;
            co_return std::optional<aid::Contact>{std::move(*picked)};
        }
    }

    // Step 2: prefix match on the companies book (Company). Trim the
    // last EXTENSION_LENGTH digits — that's the company main line.
    // Skip if the canonical number is shorter than the extension
    // length (no meaningful prefix to search by).
    if (number.v.size() > EXTENSION_LENGTH) {
        aid::PhoneNumber trimmed{number.v.substr(0, number.v.size() - EXTENSION_LENGTH)};
        auto query = buildPrefixQuery(trimmed);
        if (!query) {
            co_return aid::plumbing::unexpected{query.error()};
        }
        auto resp = co_await http_.report(cfg_.bookCompanies, *query);
        if (!resp) {
            co_return aid::plumbing::unexpected{resp.error()};
        }
        auto contacts = internal::DcVCardParser::parse(*resp);
        if (auto picked = pickLongestCommonPrefix(contacts, number)) {
            picked->kind = aid::AddressKind::Company;
            co_return std::optional<aid::Contact>{std::move(*picked)};
        }
    }

    co_return std::optional<aid::Contact>{};
}

aid::plumbing::Task<aid::plumbing::Result<void>> DaviCalAdapter::ping() {
    // Cold-start ping: auth'd PROPFIND Depth: 0 on the configured
    // addresses book URL — DaviCal answers 207 Multi-Status when reachable.
    // A plain GET on a CardDAV collection returns 405, so PROPFIND is the
    // correct cheap probe. No XML body; exercises the same auth path as the
    // real lookup flow.
    co_return co_await http_.probe(cfg_.bookAddresses);
}

} // namespace aid::adapters::davical
