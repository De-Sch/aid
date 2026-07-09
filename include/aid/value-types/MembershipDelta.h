#pragma once

#include <vector>

#include "aid/value-types/Ids.h"

// MembershipDelta — the per-project change in a project's member set between two
// observations, expressed purely in domain terms (ProjectId + UserHandle). It is
// the generic currency the membership poller speaks: an adapter diffs a freshly
// fetched member set against its cache and returns one MembershipDelta per
// project whose set changed; the core then pushes ticket_upsert to the `added`
// logins and (conditionally) ticket_remove to the `removed` logins.
//
// All ticket-system-specific workarounds (the /memberships filter, principal-href
// → login resolution, pagination) stay sealed inside the plugin — this type
// carries no backend detail.

namespace aid {

struct MembershipDelta {
    ProjectId project;
    std::vector<UserHandle> added;
    std::vector<UserHandle> removed;
};

} // namespace aid
