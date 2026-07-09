#include "aid/controllers/ControllerSupport.h"

namespace aid::controllers {

drogon::HttpStatusCode httpStatusForError(aid::plumbing::ErrorCode code) noexcept {
    using aid::plumbing::ErrorCode;
    switch (code) {
    case ErrorCode::InvalidInput:
        return drogon::k400BadRequest;
    case ErrorCode::NotFound:
        return drogon::k404NotFound;
    case ErrorCode::Conflict:
    case ErrorCode::Conflict409:
    case ErrorCode::LockVersionExhausted:
        // Local uniqueness clash, ticket-system optimistic-lock 409, and a 409 that
        // survived all 5 retries — all "your write lost a race, resubmit".
        return drogon::k409Conflict;
    case ErrorCode::Unprocessable422:
        return drogon::k422UnprocessableEntity;
    case ErrorCode::Unauthenticated:
        return drogon::k401Unauthorized;
    case ErrorCode::Forbidden:
        return drogon::k403Forbidden;
    case ErrorCode::TooManyRequests:
        return drogon::k429TooManyRequests;
    case ErrorCode::UpstreamUnavailable:
        // We are a gateway to the ticket / address systems; if the upstream is down it is
        // a 502, not a fault in this service.
        return drogon::k502BadGateway;
    case ErrorCode::UpstreamTimeout:
        return drogon::k504GatewayTimeout;
    case ErrorCode::WalWriteFailed:
    case ErrorCode::WalSyncFailed:
    case ErrorCode::PluginAbiMismatch:
    case ErrorCode::InvariantViolation:
    case ErrorCode::Unknown:
        return drogon::k500InternalServerError;
    }
    // Unreachable: every enumerator is handled above (no default, so a new
    // ErrorCode is a -Wswitch build error). Kept for -Wreturn-type.
    return drogon::k500InternalServerError;
}

} // namespace aid::controllers
