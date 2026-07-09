#include "aid/adapters/openproject/internal/paging.h"

#include <cstddef>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>

#include "aid/adapters/openproject/internal/url.h"
#include "aid/plumbing/Error.h"

using aid::plumbing::Result;
using aid::plumbing::Task;
using aid::plumbing::unexpected;

namespace aid::adapters::openproject {

Task<Result<void>> pagedGet(OpHttp& http, std::string baseUrlWithParams, int pageSize,
                            PageSink sink) {
    // Termination is driven by the collection's `total` (the authoritative match
    // count OpenProject always reports), NOT by the page size we requested. That
    // distinction matters: OpenProject CLAMPS an over-large `pageSize` down to
    // its server maximum (request 1000 → 1000, request 5000 → maybe 1000). A
    // "stop when the page is shorter than what we asked for" rule would mistake
    // such a clamped full page for the last one and silently truncate — exactly
    // the bug class this loop exists to kill. Looping until `accumulated >=
    // total` is correct for any requested size and any server cap; a short/empty
    // page is only a fallback stop for the (real-OpenProject-never) case where
    // `total` is absent. A result set within one page still costs exactly one GET.
    long long total = -1;
    std::size_t accumulated = 0;
    for (int page = 1;; ++page) {
        std::string path = baseUrlWithParams;
        path.append("&offset=");
        path.append(std::to_string(page));
        path.append("&pageSize=");
        path.append(std::to_string(pageSize));

        auto resp = co_await http.get(std::move(path));
        if (!resp)
            co_return unexpected(resp.error());

        if (auto it = resp->find("total"); it != resp->end() && it->is_number())
            total = it->template get<long long>();

        // Count what the SERVER sent (the elements array), not what the sink kept
        // — a sink that skips rows (projectMembers drops group principals) must
        // not make the loop think a full page was short and stop early.
        std::size_t got = 0;
        if (auto embIt = resp->find("_embedded"); embIt != resp->end() && embIt->is_object()) {
            if (auto elIt = embIt->find("elements"); elIt != embIt->end() && elIt->is_array()) {
                got = elIt->size();
                auto sunk = sink(*elIt);
                if (!sunk)
                    co_return unexpected(sunk.error());
            }
        }

        accumulated += got;
        // Authoritative stop: every matching row has been collected.
        if (total >= 0 && static_cast<long long>(accumulated) >= total)
            break;
        // Fallbacks (only when `total` was never reported): an empty page means
        // nothing more to read; a short page means OpenProject honoured our full
        // request and had no more rows.
        if (got == 0)
            break;
        if (total < 0 && got < static_cast<std::size_t>(pageSize))
            break;
    }
    co_return Result<void>{};
}

std::string sortByIdAscParam() {
    return "&sortBy=" + urlEncode(R"([["id","asc"]])");
}

} // namespace aid::adapters::openproject
