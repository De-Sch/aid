#pragma once

#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "aid/adapters/openproject/internal/OpHttp.h"
#include "aid/plumbing/Result.h"
#include "aid/plumbing/Task.h"
#include "aid/value-types/Ids.h"
#include "aid/value-types/MembershipDelta.h"

// OpUserRepo — OpenProject user lookup + project-membership queries.
//
//   resolveLogin   : login → UserHandle (nullopt on miss)
//   hrefFor        : UserHandle → /api/v3/users/<numericId>, with cache
//   projectsForUser: UserHandle → list of ProjectIds where the user is principal
//   projectMembers : ProjectId → list of member UserHandles (inverse of
//                    projectsForUser); cached per project
//
// hrefCache_/loginByHref_ are process-lifetime: OpenProject's user↔href mapping
// is stable for the daemon's lifetime, so we never invalidate it. membersCache_
// is NOT process-lifetime — project membership can change mid-run (an admin
// adds/removes a member), and OpenProject emits no membership webhook, so
// refreshMembership() re-polls and swaps each cached project's member set on the
// poll loop (infrastructure/MembershipReconciler).
//
// All three maps are guarded by cacheMtx_. The port contract requires every
// method to be safe to call concurrently — the
// daemon no longer pins all callers to the single domain loop (the /ui/* paths
// resume the request on the connection's loop), so the
// "single-loop ⇒ no mutex" assumption no longer holds. The lock is taken only
// around each map read/write and is NEVER held across a co_await: between a
// cache miss and the emplace the map is unlocked, so the worst case stays a
// harmless duplicate fetch (the second emplace is a no-op), but the maps
// themselves can never be mutated while another thread holds an iterator into
// them (mirrors ProducedLedger/HandlerLedger).

namespace aid::adapters::openproject {

class OpUserRepo {
public:
    explicit OpUserRepo(OpHttp& http) noexcept : http_(http) {}

    OpUserRepo(const OpUserRepo&) = delete;
    OpUserRepo& operator=(const OpUserRepo&) = delete;
    OpUserRepo(OpUserRepo&&) = delete;
    OpUserRepo& operator=(OpUserRepo&&) = delete;
    ~OpUserRepo() = default;

    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<std::optional<aid::UserHandle>>>
    resolveLogin(std::string_view login);

    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<std::string>>
    hrefFor(aid::UserHandle user);

    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<std::vector<aid::ProjectId>>>
    projectsForUser(aid::UserHandle user);

    // The member logins of `project` — GET
    // /api/v3/memberships?filters=[{"project":{"operator":"=","values":["<id>"]}}].
    // (The /api/v3/projects/{id}/members route does NOT exist in OpenProject v3 —
    // it 404s — so we query the memberships collection filtered by project.) Each
    // membership names a principal by href only, so every user principal is
    // resolved to its login via loginForUserHref; group/placeholder principals
    // carry no login and are skipped. This is the inverse of projectsForUser and
    // drives TicketStore::recipientsFor / dashboard visibility. Result populates
    // membersCache_ for this project; the entry is thereafter kept fresh by
    // refreshMembership() (the poll loop), not assumed stable for the run.
    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<std::vector<aid::UserHandle>>>
    projectMembers(aid::ProjectId project);

    // Re-poll the membership of every project already in membersCache_, diff each
    // fresh set against the cached one, swap the cache to the fresh sets, and
    // return one MembershipDelta per project whose set changed. Nothing cached ⇒
    // empty result (no project has been resolved yet, so there is nothing to
    // reconcile). Cost: one batched GET
    // /api/v3/memberships?filters=[{"project":{"operator":"=","values":[<all
    // cached ids>]}}] (paginated like getAllPaged) plus one GET per *newly seen*
    // principal — already-known logins are served from loginByHref_, so a steady
    // state with no joins costs exactly the batched GET(s).
    //
    // SAFETY GUARD: a failed batched fetch (or a wholly-empty response while the
    // cache is non-empty — the signature of lost API-token permission) is read
    // as NO CHANGE: the cache is kept and no delta is produced. An error must
    // never be turned into "all members removed". A per-principal resolve failure
    // taints only that principal's project, which is then left untouched.
    //
    // The diff-and-swap runs strictly synchronously (no co_await between reading
    // the old cached set and writing the fresh one) so the mutex-free
    // single-domain-loop cache invariant holds.
    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<std::vector<aid::MembershipDelta>>>
    refreshMembership();

private:
    // Resolve a principal href ("/api/v3/users/<id>") to its login by GETting it
    // and reading the `login` field. nullopt when the principal exposes no login
    // (e.g. a group, or a deactivated principal). Caches by href; also warms
    // hrefCache_ in reverse so a later hrefFor(login) is a hit.
    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<std::optional<aid::UserHandle>>>
    loginForUserHref(std::string_view href);

    OpHttp& http_;
    // Guards the three caches below. Locked only around each map access, never
    // across a co_await (see the class comment).
    std::mutex cacheMtx_;
    std::unordered_map<aid::UserHandle, std::string> hrefCache_;
    std::unordered_map<std::string, aid::UserHandle> loginByHref_;
    std::unordered_map<aid::ProjectId, std::vector<aid::UserHandle>> membersCache_;
};

} // namespace aid::adapters::openproject
