#include "aid/adapters/openproject/internal/OpUserRepo.h"

#include <cstddef>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "aid/adapters/openproject/internal/url.h"
#include "aid/plumbing/Error.h"

using aid::plumbing::Error;
using aid::plumbing::ErrorCode;
using aid::plumbing::Result;
using aid::plumbing::Task;
using aid::plumbing::unexpected;

namespace aid::adapters::openproject {

Task<Result<std::optional<aid::UserHandle>>> OpUserRepo::resolveLogin(std::string_view login) {
    const std::string url = singleFilterUrl("/api/v3/users", "login", "=", login);
    auto resp = co_await http_.get(url);
    if (!resp) {
        co_return unexpected(resp.error());
    }

    // HAL response: _embedded.elements is the array of user objects.
    auto embIt = resp->find("_embedded");
    if (embIt == resp->end() || !embIt->is_object()) {
        co_return std::optional<aid::UserHandle>{};
    }
    auto elIt = embIt->find("elements");
    if (elIt == embIt->end() || !elIt->is_array() || elIt->empty()) {
        co_return std::optional<aid::UserHandle>{};
    }

    // First element wins — login is unique in OpenProject.
    const auto& user = (*elIt)[0];
    std::string handle;
    if (auto lg = user.find("login"); lg != user.end() && lg->is_string()) {
        handle = lg->get<std::string>();
    } else {
        // Defensive: filter matched but the row has no login field. Treat
        // as a miss rather than fabricating a handle from id/href.
        co_return std::optional<aid::UserHandle>{};
    }

    // Warm the href cache too — the very next call is almost certainly
    // hrefFor(handle), so doing it now avoids a second round-trip.
    if (auto links = user.find("_links"); links != user.end() && links->is_object()) {
        if (auto self = links->find("self"); self != links->end() && self->is_object()) {
            if (auto href = self->find("href"); href != self->end() && href->is_string()) {
                std::scoped_lock lk{cacheMtx_};
                hrefCache_.emplace(aid::UserHandle{handle}, href->get<std::string>());
            }
        }
    }

    co_return std::optional<aid::UserHandle>{aid::UserHandle{std::move(handle)}};
}

Task<Result<std::string>> OpUserRepo::hrefFor(aid::UserHandle user) {
    {
        std::scoped_lock lk{cacheMtx_};
        if (auto it = hrefCache_.find(user); it != hrefCache_.end()) {
            co_return it->second;
        }
    }

    const std::string url = singleFilterUrl("/api/v3/users", "login", "=", user.v);
    auto resp = co_await http_.get(url);
    if (!resp) {
        co_return unexpected(resp.error());
    }
    auto embIt = resp->find("_embedded");
    if (embIt == resp->end() || !embIt->is_object()) {
        co_return unexpected(Error{ErrorCode::NotFound,
                                   "OpUserRepo::hrefFor: no _embedded for login " + user.v,
                                   std::nullopt});
    }
    auto elIt = embIt->find("elements");
    if (elIt == embIt->end() || !elIt->is_array() || elIt->empty()) {
        co_return unexpected(Error{ErrorCode::NotFound,
                                   "OpUserRepo::hrefFor: no users matched login " + user.v,
                                   std::nullopt});
    }

    const auto& u = (*elIt)[0];
    auto links = u.find("_links");
    if (links == u.end() || !links->is_object()) {
        co_return unexpected(Error{ErrorCode::Unknown,
                                   "OpUserRepo::hrefFor: missing _links for login " + user.v,
                                   std::nullopt});
    }
    auto self = links->find("self");
    if (self == links->end() || !self->is_object()) {
        co_return unexpected(Error{ErrorCode::Unknown,
                                   "OpUserRepo::hrefFor: missing self link for login " + user.v,
                                   std::nullopt});
    }
    auto href = self->find("href");
    if (href == self->end() || !href->is_string()) {
        co_return unexpected(Error{ErrorCode::Unknown,
                                   "OpUserRepo::hrefFor: missing self.href for login " + user.v,
                                   std::nullopt});
    }

    std::string h = href->get<std::string>();
    {
        std::scoped_lock lk{cacheMtx_};
        hrefCache_.emplace(user, h);
    }
    co_return h;
}

Task<Result<std::vector<aid::ProjectId>>> OpUserRepo::projectsForUser(aid::UserHandle user) {
    auto href = co_await hrefFor(user);
    if (!href)
        co_return unexpected(href.error());

    // Filter shape: [{"principal":{"operator":"=","values":["<userId>"]}}]
    // The principal filter takes the NUMERIC user id, not the full href —
    // OpenProject rejects "/api/v3/users/4" with HTTP 400 ("Principal
    // filter has invalid values"). Strip to the trailing id ("4").
    const std::string url = singleFilterUrl("/api/v3/projects", "principal", "=", hrefTail(*href));
    auto resp = co_await http_.get(url);
    if (!resp)
        co_return unexpected(resp.error());

    std::vector<aid::ProjectId> ids;
    auto embIt = resp->find("_embedded");
    if (embIt == resp->end() || !embIt->is_object())
        co_return ids;
    auto elIt = embIt->find("elements");
    if (elIt == embIt->end() || !elIt->is_array())
        co_return ids;

    ids.reserve(elIt->size());
    for (const auto& p : *elIt) {
        // Prefer top-level "id" (HAL exposes it). Fall back to _links.self.href tail.
        if (auto idIt = p.find("id"); idIt != p.end()) {
            if (idIt->is_number_integer()) {
                ids.emplace_back(aid::ProjectId{std::to_string(idIt->get<long long>())});
                continue;
            }
            if (idIt->is_string()) {
                ids.emplace_back(aid::ProjectId{idIt->get<std::string>()});
                continue;
            }
        }
        if (auto links = p.find("_links"); links != p.end() && links->is_object()) {
            if (auto self = links->find("self"); self != links->end() && self->is_object()) {
                if (auto hr = self->find("href"); hr != self->end() && hr->is_string()) {
                    ids.emplace_back(aid::ProjectId{hrefTail(hr->get<std::string>())});
                }
            }
        }
    }
    co_return ids;
}

Task<Result<std::optional<aid::UserHandle>>> OpUserRepo::loginForUserHref(std::string_view href) {
    const std::string key{href};
    {
        std::scoped_lock lk{cacheMtx_};
        if (auto it = loginByHref_.find(key); it != loginByHref_.end())
            co_return std::optional<aid::UserHandle>{it->second};
    }

    auto resp = co_await http_.get(key);
    if (!resp)
        co_return unexpected(resp.error());

    auto lg = resp->find("login");
    if (lg == resp->end() || !lg->is_string()) {
        // Principal exposes no login (group / placeholder / hidden). Treat as a
        // miss rather than fabricating a handle from the numeric id — a numeric
        // handle would never match the session login the WS hub keys on.
        co_return std::optional<aid::UserHandle>{};
    }

    aid::UserHandle handle{lg->get<std::string>()};
    {
        std::scoped_lock lk{cacheMtx_};
        loginByHref_.emplace(key, handle);
        hrefCache_.emplace(handle, key); // reverse-warm: hrefFor(handle) is now a hit
    }
    co_return std::optional<aid::UserHandle>{handle};
}

Task<Result<std::vector<aid::UserHandle>>> OpUserRepo::projectMembers(aid::ProjectId project) {
    // Cache hit: a project resolved once is served from cache until
    // refreshMembership swaps in a fresh set.
    {
        std::scoped_lock lk{cacheMtx_};
        if (auto it = membersCache_.find(project); it != membersCache_.end())
            co_return it->second;
    }

    // GET /api/v3/memberships?filters=[{"project":{"operator":"=","values":["<id>"]}}].
    // The older /api/v3/projects/<id>/members route does not exist in OpenProject
    // v3 (404), so we filter the memberships collection by project. The project
    // filter takes the numeric project id, which is exactly project.v.
    const std::string url = singleFilterUrl("/api/v3/memberships", "project", "=", project.v);
    auto resp = co_await http_.get(url);
    if (!resp)
        co_return unexpected(resp.error());

    // Collect principal hrefs first so no json iterator is held across the
    // per-principal co_await below.
    std::vector<std::string> principalHrefs;
    if (auto embIt = resp->find("_embedded"); embIt != resp->end() && embIt->is_object()) {
        if (auto elIt = embIt->find("elements"); elIt != embIt->end() && elIt->is_array()) {
            principalHrefs.reserve(elIt->size());
            for (const auto& el : *elIt) {
                auto links = el.find("_links");
                if (links == el.end() || !links->is_object())
                    continue;
                auto pr = links->find("principal");
                if (pr == links->end() || !pr->is_object())
                    continue;
                if (auto hr = pr->find("href"); hr != pr->end() && hr->is_string())
                    principalHrefs.push_back(hr->get<std::string>());
            }
        }
    }

    // Resolve each user principal to its login so the result unions cleanly with
    // the callHandler CSV (logins) and matches the session handle the WS hub keys
    // on. Only user principals carry a login; skip group/placeholder principals.
    // Dedup so a user who is a member through several roles appears once.
    std::vector<aid::UserHandle> members;
    std::unordered_set<std::string> seen;
    members.reserve(principalHrefs.size());
    for (const auto& href : principalHrefs) {
        if (href.find("/users/") == std::string::npos)
            continue; // group / placeholder principal — no login to notify
        auto login = co_await loginForUserHref(href);
        if (!login)
            co_return unexpected(login.error());
        if (login->has_value() && seen.insert((*login)->v).second)
            members.push_back(**login);
    }

    {
        std::scoped_lock lk{cacheMtx_};
        membersCache_.emplace(project, members);
    }
    co_return members;
}

Task<Result<std::vector<aid::MembershipDelta>>> OpUserRepo::refreshMembership() {
    // OpenProject's server page cap; an over-large pageSize is clamped, so we
    // page and stop on the collection's authoritative `total` (same discipline
    // as OpTicketRepo::getAllPaged).
    constexpr int kMembershipPageSize = 200;

    // Snapshot the projects we already track. A project enters membersCache_
    // only via projectMembers; if nothing has been resolved yet there is
    // nothing to reconcile.
    std::vector<aid::ProjectId> projects;
    {
        std::scoped_lock lk{cacheMtx_};
        projects.reserve(membersCache_.size());
        for (const auto& kv : membersCache_)
            projects.push_back(kv.first);
    }
    if (projects.empty())
        co_return std::vector<aid::MembershipDelta>{};

    // One batched filter for every tracked project:
    // [{"project":{"operator":"=","values":["<id>","<id>",…]}}].
    nlohmann::json values = nlohmann::json::array();
    for (const auto& p : projects)
        values.push_back(p.v);
    nlohmann::json filters = nlohmann::json::array();
    filters.push_back({{"project", {{"operator", "="}, {"values", std::move(values)}}}});
    const std::string base = multiFilterUrl("/api/v3/memberships", filters);

    // Page through the collection, accumulating (project, principal-href) rows.
    // Each membership names its project and principal by href only.
    struct Row {
        aid::ProjectId project;
        std::string principalHref;
    };
    std::vector<Row> rows;
    long long total = -1;
    std::size_t accumulated = 0;
    for (int page = 1;; ++page) {
        std::string path = base;
        path.append("&offset=");
        path.append(std::to_string(page));
        path.append("&pageSize=");
        path.append(std::to_string(kMembershipPageSize));

        auto resp = co_await http_.get(path);
        if (!resp)
            // SAFETY GUARD: a failed fetch ⇒ NO change. Keep every cached entry
            // and emit no delta — never read a transport/HTTP error as "all
            // members removed".
            co_return std::vector<aid::MembershipDelta>{};

        if (auto it = resp->find("total"); it != resp->end() && it->is_number())
            total = it->template get<long long>();

        std::size_t got = 0;
        if (auto embIt = resp->find("_embedded"); embIt != resp->end() && embIt->is_object()) {
            if (auto elIt = embIt->find("elements"); elIt != embIt->end() && elIt->is_array()) {
                got = elIt->size();
                for (const auto& el : *elIt) {
                    auto links = el.find("_links");
                    if (links == el.end() || !links->is_object())
                        continue;
                    std::string projId;
                    if (auto pj = links->find("project"); pj != links->end() && pj->is_object()) {
                        if (auto hr = pj->find("href"); hr != pj->end() && hr->is_string())
                            projId = hrefTail(hr->get<std::string>());
                    }
                    std::string prinHref;
                    if (auto pr = links->find("principal"); pr != links->end() && pr->is_object()) {
                        if (auto hr = pr->find("href"); hr != pr->end() && hr->is_string())
                            prinHref = hr->get<std::string>();
                    }
                    if (!projId.empty() && !prinHref.empty())
                        rows.push_back({aid::ProjectId{std::move(projId)}, std::move(prinHref)});
                }
            }
        }

        accumulated += got;
        if (total >= 0 && static_cast<long long>(accumulated) >= total)
            break;
        if (got == 0)
            break;
        if (total < 0 && got < static_cast<std::size_t>(kMembershipPageSize))
            break;
    }

    // SAFETY GUARD: a wholly-empty response while we still track projects is the
    // signature of lost API-token permission, NOT every project simultaneously
    // losing every member. Treat it as no change.
    if (rows.empty())
        co_return std::vector<aid::MembershipDelta>{};

    // Build the fresh per-project login set. Seed every queried project with an
    // empty set so a project that lost its *last* member still diffs against a
    // present-but-empty fresh set. Resolve each user principal to its login;
    // warmed logins (every prior member) are loginByHref_ hits ⇒ 0 GETs.
    std::unordered_map<aid::ProjectId, std::vector<aid::UserHandle>> fresh;
    std::unordered_map<aid::ProjectId, std::unordered_set<std::string>> seen;
    std::unordered_set<aid::ProjectId> tainted;
    fresh.reserve(projects.size());
    seen.reserve(projects.size());
    for (const auto& p : projects) {
        fresh.emplace(p, std::vector<aid::UserHandle>{});
        seen.emplace(p, std::unordered_set<std::string>{});
    }
    for (auto& row : rows) {
        if (row.principalHref.find("/users/") == std::string::npos)
            continue; // group / placeholder principal — no login to notify
        auto login = co_await loginForUserHref(row.principalHref);
        if (!login) {
            // Per-principal resolve failed: do not risk reading a transient
            // error as a removal. Taint this project so it is left untouched
            // (cache kept, no delta) this cycle.
            tainted.insert(row.project);
            continue;
        }
        if (login->has_value() && seen[row.project].insert((*login)->v).second)
            fresh[row.project].push_back(**login);
    }

    // Diff + cache swap — strictly synchronous from here: NO co_await between
    // reading a project's old cached set and writing its fresh one. The whole
    // block runs under cacheMtx_ (no suspension inside) so the read-old/
    // write-fresh pair is atomic against a concurrent projectMembers/hrefFor.
    std::vector<aid::MembershipDelta> deltas;
    std::scoped_lock lk{cacheMtx_};
    for (const auto& p : projects) {
        if (tainted.count(p) != 0)
            continue; // resolve error for this project → keep cache, no delta

        auto cacheIt = membersCache_.find(p);
        if (cacheIt == membersCache_.end())
            continue; // defensive: vanished mid-flight

        std::vector<aid::UserHandle>& freshSet = fresh[p];

        std::unordered_set<std::string> oldLogins;
        oldLogins.reserve(cacheIt->second.size());
        for (const auto& u : cacheIt->second)
            oldLogins.insert(u.v);
        std::unordered_set<std::string> newLogins;
        newLogins.reserve(freshSet.size());
        for (const auto& u : freshSet)
            newLogins.insert(u.v);

        aid::MembershipDelta delta;
        delta.project = p;
        for (const auto& u : freshSet)
            if (oldLogins.count(u.v) == 0)
                delta.added.push_back(u);
        for (const auto& u : cacheIt->second)
            if (newLogins.count(u.v) == 0)
                delta.removed.push_back(u);

        // Swap in the fresh set (idempotent when unchanged); emit a delta only
        // when the set actually moved.
        cacheIt->second = std::move(freshSet);
        if (!delta.added.empty() || !delta.removed.empty())
            deltas.push_back(std::move(delta));
    }
    co_return deltas;
}

} // namespace aid::adapters::openproject
