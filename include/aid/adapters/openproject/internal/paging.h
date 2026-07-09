#pragma once

#include <functional>
#include <nlohmann/json_fwd.hpp>
#include <string>

#include "aid/adapters/openproject/internal/OpHttp.h"
#include "aid/plumbing/Result.h"
#include "aid/plumbing/Task.h"

// Shared HAL-collection pagination for every OpenProject list query.
//
// OpenProject's API v3 paginates a HAL collection via `offset` (the 1-based
// page NUMBER, not a row offset) and `pageSize`, and reports the authoritative
// match count as `total`. Its default `pageSize` when the parameter is omitted
// is **20** — an un-paged GET against any collection endpoint therefore
// silently returns the first 20 rows and nothing else. That has bitten this
// adapter twice now (work_packages, then /projects + /memberships), so the loop
// lives in exactly one place.
//
// Callers pass a URL that already carries `?filters=…` (and any `&sortBy=…`);
// pagedGet splices on `&offset=` + `&pageSize=` and drives the loop.
//
// See OpTicketRepo::getAllPaged, OpUserRepo::projectsForUser /
// projectMembers / refreshMembership.

namespace aid::adapters::openproject {

// Called once per fetched page with that page's `_embedded.elements` array.
//
// Invoked synchronously between the co_awaits of the paging loop, never across
// one — so a sink may hold json iterators into `elements` for its duration, but
// must NOT itself co_await. Returning an Error aborts the loop and propagates
// (used by the ticket sink, whose per-element parse can fail).
using PageSink = std::function<aid::plumbing::Result<void>(const nlohmann::json& elements)>;

// GET every page of the collection at `baseUrlWithParams`, handing each page's
// elements to `sink`. Both value parameters are owned by the coroutine frame:
// they are read after the first suspension point, so a borrowed view would
// dangle (the same reason OpHttp::get takes its path by value).
[[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<void>>
pagedGet(OpHttp& http, std::string baseUrlWithParams, int pageSize, PageSink sink);

// "&sortBy=<encoded [["id","asc"]]>" — for collections whose DEFAULT order is
// id-DESCENDING (/api/v3/projects and /api/v3/memberships both are; verified
// against a live OpenProject). Under descending order a row created between two
// page fetches shifts every later row one place right, so a row at a page
// boundary is silently skipped. Ascending order makes an insert append at the
// tail, where it cannot displace anything already paged past.
//
// Deliberately NOT applied to the work_packages queries: those URLs are
// live-verified as they stand, and the dashboard re-sorts the merged result in
// OpDashboardBuilder anyway, so adding it there would be churn on a proven path.
[[nodiscard]] std::string sortByIdAscParam();

} // namespace aid::adapters::openproject
