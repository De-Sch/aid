#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// Header-only helpers shared by the OpenProject and DaviCal plugin .so files.
//
// ABI note: plugins may not link a shared *static/shared* library of daemon
// code (a `.so` links only aid_ports + stdlib +
// its own third-party deps). These helpers are therefore pure-stdlib `inline`
// functions in a header: each plugin compiles its OWN copy into its `.so`, so
// nothing crosses the extern "C" boundary and no symbol is shared at link time.
// That is exactly why they live here rather than in a linked library.
//
// Extracted from previously copy-pasted bodies in OpHttp.cpp / DcHttp.cpp
// (base64) and OpenProjectAdapter.cpp / factory.cpp (URL scheme+host). The
// extraction is behaviour-preserving; callers keep any flow-specific policy
// (e.g. the no-scheme fallback) on their side via schemeAndHost's optional.
namespace aid::adapters::support {

namespace detail {

// Canonical RFC 4648 §4 base64 alphabet ('+/' , '=' padding).
inline constexpr std::array<char, 64> kBase64Alphabet{
    'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P',
    'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', 'a', 'b', 'c', 'd', 'e', 'f',
    'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v',
    'w', 'x', 'y', 'z', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '+', '/'};

} // namespace detail

// RFC 4648 §4 base64 encode. Standard alphabet, '=' padding, no line wrapping.
// Hand-rolled so a plugin needs no extra dependency just for a Basic-auth
// header. Output is byte-identical to the two former copies for every input.
[[nodiscard]] inline std::string base64Encode(std::string_view in) {
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    std::size_t i = 0;
    while (i + 3 <= in.size()) {
        const auto b0 = static_cast<std::uint8_t>(in[i]);
        const auto b1 = static_cast<std::uint8_t>(in[i + 1]);
        const auto b2 = static_cast<std::uint8_t>(in[i + 2]);
        out.push_back(detail::kBase64Alphabet[b0 >> 2]);
        out.push_back(detail::kBase64Alphabet[((b0 & 0x03U) << 4U) | (b1 >> 4U)]);
        out.push_back(detail::kBase64Alphabet[((b1 & 0x0FU) << 2U) | (b2 >> 6U)]);
        out.push_back(detail::kBase64Alphabet[b2 & 0x3FU]);
        i += 3;
    }
    if (i < in.size()) {
        const auto b0 = static_cast<std::uint8_t>(in[i]);
        out.push_back(detail::kBase64Alphabet[b0 >> 2]);
        if (i + 1 == in.size()) {
            out.push_back(detail::kBase64Alphabet[(b0 & 0x03U) << 4U]);
            out.push_back('=');
            out.push_back('=');
        } else {
            const auto b1 = static_cast<std::uint8_t>(in[i + 1]);
            out.push_back(detail::kBase64Alphabet[((b0 & 0x03U) << 4U) | (b1 >> 4U)]);
            out.push_back(detail::kBase64Alphabet[(b1 & 0x0FU) << 2U]);
            out.push_back('=');
        }
    }
    return out;
}

// Extract "scheme://host[:port]" from a full URL — i.e. everything up to (but
// not including) the first '/' after the scheme. Returns nullopt when there is
// no http(s):// scheme, so each caller can apply its own no-scheme policy:
//   - OpenProject uses the raw string as-is:  schemeAndHost(u).value_or(std::string{u})
//   - DaviCal treats it as a config error:    schemeAndHost(u).value_or(std::string{})
[[nodiscard]] inline std::optional<std::string> schemeAndHost(std::string_view url) {
    constexpr std::string_view kHttps = "https://";
    constexpr std::string_view kHttp = "http://";
    std::string_view scheme;
    std::string_view rest;
    if (url.starts_with(kHttps)) {
        scheme = kHttps;
        rest = url.substr(kHttps.size());
    } else if (url.starts_with(kHttp)) {
        scheme = kHttp;
        rest = url.substr(kHttp.size());
    } else {
        return std::nullopt;
    }
    const auto slash = rest.find('/');
    const auto host = (slash == std::string_view::npos) ? rest : rest.substr(0, slash);
    return std::string{scheme} + std::string{host};
}

} // namespace aid::adapters::support
