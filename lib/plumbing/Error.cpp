#include "aid/plumbing/Error.h"

namespace aid::plumbing {

std::string_view errorCodeName(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::InvalidInput:
        return "InvalidInput";
    case ErrorCode::NotFound:
        return "NotFound";
    case ErrorCode::Conflict:
        return "Conflict";
    case ErrorCode::Conflict409:
        return "Conflict409";
    case ErrorCode::Unprocessable422:
        return "Unprocessable422";
    case ErrorCode::LockVersionExhausted:
        return "LockVersionExhausted";
    case ErrorCode::UpstreamUnavailable:
        return "UpstreamUnavailable";
    case ErrorCode::UpstreamTimeout:
        return "UpstreamTimeout";
    case ErrorCode::Unauthenticated:
        return "Unauthenticated";
    case ErrorCode::Forbidden:
        return "Forbidden";
    case ErrorCode::TooManyRequests:
        return "TooManyRequests";
    case ErrorCode::WalWriteFailed:
        return "WalWriteFailed";
    case ErrorCode::WalSyncFailed:
        return "WalSyncFailed";
    case ErrorCode::PluginAbiMismatch:
        return "PluginAbiMismatch";
    case ErrorCode::InvariantViolation:
        return "InvariantViolation";
    case ErrorCode::Unknown:
        return "Unknown";
    }
    return "Unknown";
}

} // namespace aid::plumbing
