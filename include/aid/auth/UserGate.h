#pragma once

#include <optional>

#include "aid/value-types/Ids.h"

namespace aid::auth {

class UserRepo;

// Authorization precondition for inbound /call events: is this phone-side
// operator handle a user of our system (present in auth.db's `users` table)?
//
// Used at the mailbox dispatch seam (src/main.cpp) to gate the three
// user-bearing call events (Outgoing, Transfer, and Accepted-when-present):
// an unknown handle means the whole event is silently dropped before any
// use case runs.
//
// Semantics:
//   * absent handle (std::nullopt)      -> true  (event carries no user; not gated)
//   * handle present AND in auth.db     -> true  (proceed with the workflow)
//   * handle present but NOT in auth.db -> false (drop)
//   * handle present but the DB lookup ERRORED -> false (fail-closed: a
//     precondition we cannot confirm is treated as unmet). auth.db is a local
//     SQLite file, so an error here is systemic, not transient.
//
// The match is exact against users.username (UserRepo::lookupByUsername). Note
// the call bridge lowercases the handle before sending it, so operator
// accounts in auth.db must be created lowercase to match.
[[nodiscard]] bool userKnown(const UserRepo& repo, const std::optional<aid::UserHandle>& handle);

// Convenience overload for the events whose handle is required (non-optional):
// OutgoingCall::user, TransferCall::newUser.
[[nodiscard]] bool userKnown(const UserRepo& repo, const aid::UserHandle& handle);

} // namespace aid::auth
