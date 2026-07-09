#pragma once

#include <cstdint>
#include <string>

#include "aid/value-types/Ids.h"

namespace aid::plumbing {

struct WalRecord {
    std::uint64_t seq{0};
    aid::Timestamp receivedAt{};
    std::string correlationId;
    std::string body;
};

} // namespace aid::plumbing
