#include "aid/crosscutting/CorrelationId.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <random>

namespace aid::crosscutting {

namespace {

// Thread-local PRNG seeded once per thread from std::random_device.
// No syscalls per nextUuid() call — meets the "sub-microsecond" target.
std::mt19937_64& threadRng() {
    thread_local std::mt19937_64 rng = [] {
        std::random_device rd;
        // 128 bits of seed material: two 64-bit draws.
        std::seed_seq seq{rd(), rd(), rd(), rd(), rd(), rd(), rd(), rd()};
        return std::mt19937_64{seq};
    }();
    return rng;
}

constexpr char hex(unsigned v) noexcept {
    return static_cast<char>(v < 10 ? '0' + static_cast<int>(v) : 'a' + static_cast<int>(v) - 10);
}

} // namespace

std::string CorrelationId::nextUuid() {
    auto& rng = threadRng();
    std::uint64_t hi = rng();
    std::uint64_t lo = rng();

    // RFC 4122 §4.4 — version 4 in the high nibble of byte 6
    // (i.e. nibble 13 of the 32 hex chars), variant 10b in the high
    // bits of byte 8 (nibble 17).
    // hi = aaaaaaaa-aaaa-Mxxx (M = 4 in nibble 13, counting from 0)
    hi = (hi & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
    // lo = Nxxx-xxxxxxxxxxxx (N high two bits = 10)
    lo = (lo & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;

    std::array<char, 36> buf{};
    auto write_byte = [&](std::size_t pos, unsigned byte) {
        buf[pos] = hex((byte >> 4) & 0xF);
        buf[pos + 1] = hex(byte & 0xF);
    };

    // Layout: 8-4-4-4-12 with hyphens at 8, 13, 18, 23.
    const std::array<std::uint8_t, 16> bytes = {
        static_cast<std::uint8_t>((hi >> 56) & 0xFF), static_cast<std::uint8_t>((hi >> 48) & 0xFF),
        static_cast<std::uint8_t>((hi >> 40) & 0xFF), static_cast<std::uint8_t>((hi >> 32) & 0xFF),
        static_cast<std::uint8_t>((hi >> 24) & 0xFF), static_cast<std::uint8_t>((hi >> 16) & 0xFF),
        static_cast<std::uint8_t>((hi >> 8) & 0xFF),  static_cast<std::uint8_t>(hi & 0xFF),
        static_cast<std::uint8_t>((lo >> 56) & 0xFF), static_cast<std::uint8_t>((lo >> 48) & 0xFF),
        static_cast<std::uint8_t>((lo >> 40) & 0xFF), static_cast<std::uint8_t>((lo >> 32) & 0xFF),
        static_cast<std::uint8_t>((lo >> 24) & 0xFF), static_cast<std::uint8_t>((lo >> 16) & 0xFF),
        static_cast<std::uint8_t>((lo >> 8) & 0xFF),  static_cast<std::uint8_t>(lo & 0xFF),
    };

    // Hex positions: 0,2,4,6,-,9,11,-,14,16,-,19,21,-,24,26,28,30,32,34
    constexpr std::array<std::size_t, 16> pos = {0,  2,  4,  6,  9,  11, 14, 16,
                                                 19, 21, 24, 26, 28, 30, 32, 34};
    for (std::size_t i = 0; i < 16; ++i) {
        write_byte(pos[i], bytes[i]);
    }
    buf[8] = '-';
    buf[13] = '-';
    buf[18] = '-';
    buf[23] = '-';

    return std::string{buf.data(), buf.size()};
}

} // namespace aid::crosscutting
