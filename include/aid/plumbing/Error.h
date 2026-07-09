#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace aid::plumbing {

enum class ErrorCode {
    InvalidInput,
    NotFound,
    Conflict,    // local-side uniqueness violation (e.g. UserRepo::create on duplicate username)
    Conflict409, // ticket-system optimistic-lock conflict (upstream HTTP 409)
    Unprocessable422, // ticket-system validation reject (HTTP 422) — e.g. assigning a work
                      // package to a user who is not a member of its project. The accept/
                      // transfer save path tolerates this on the assignee link: the
                      // callHandler CSV, not the single assignee, is the real visibility
                      // mechanism, so the save still succeeds without the assignee.
    LockVersionExhausted,
    UpstreamUnavailable,
    UpstreamTimeout,
    Unauthenticated,
    Forbidden,
    TooManyRequests, // auth: the AuthService Argon2 concurrency cap rejected this request
                     // outright. Maps to HTTP 429; keeps the daemon from being OOM'd by a
                     // login-spam attacker since each Argon2id verify costs ~256 MiB of RAM.
    WalWriteFailed,
    WalSyncFailed,
    PluginAbiMismatch,
    InvariantViolation,
    Unknown,
};

struct Error {
    ErrorCode code;
    std::string message;
    std::optional<std::string> correlationId;
};

[[nodiscard]] std::string_view errorCodeName(ErrorCode code) noexcept;

} // namespace aid::plumbing
