#include "aid/adapters/openproject/internal/payload.h"

#include <chrono>
#include <cstddef>
#include <ctime>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aid/adapters/openproject/internal/url.h"
#include "aid/plumbing/Error.h"

using aid::plumbing::Error;
using aid::plumbing::ErrorCode;
using aid::plumbing::Result;
using aid::plumbing::unexpected;

namespace aid::adapters::openproject {

namespace {

Error makeInvalid(std::string msg) {
    return Error{ErrorCode::InvalidInput, std::move(msg), std::nullopt};
}

// Parse OpenProject's ISO-8601 "YYYY-MM-DDTHH:MM:SSZ" or with fractional
// seconds (e.g. "2024-01-15T10:30:00.000Z"). Returns nullopt on any parse
// failure. We deliberately parse to UTC; OpenProject always serializes UTC.
std::optional<aid::Timestamp> parseIso8601Utc(std::string_view s) {
    if (s.empty())
        return std::nullopt;

    // Trim a trailing 'Z' or fractional-second suffix before .***
    std::string head;
    head.reserve(s.size());
    for (char c : s) {
        if (c == '.' || c == 'Z')
            break;
        head.push_back(c);
    }

    std::tm tm{};
    std::istringstream is(head);
    is >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
    if (is.fail())
        return std::nullopt;

    // timegm() interprets as UTC. (gmtime/timegm pair, not localtime.)
    const std::time_t tt = ::timegm(&tm);
    if (tt == static_cast<std::time_t>(-1))
        return std::nullopt;
    return std::chrono::system_clock::from_time_t(tt);
}

// Parse a custom-field timestamp. OpenProject is inconsistent here:
// some installs serve callStart/callEnd as full ISO-8601
// ("2024-01-15T10:30:00Z", UTC with a 'Z'), others as the plain
// "YYYY-MM-DD HH:MM:SS" form. The plain form is the
// daemon's LOCAL wall-clock (same basis as the callLength breadcrumb,
// CallLineFormatter); parse it with mktime so it round-trips that local
// value. tm_isdst = -1 lets mktime consult the local TZ database (the same
// critical line as CallLineFormatter). The TZ itself is the machine's — set
// per-deployment via the environment, never hardcoded here.
std::optional<aid::Timestamp> parseCustomFieldTimestamp(std::string_view s) {
    if (auto ts = parseIso8601Utc(s); ts)
        return ts;
    std::tm tm{};
    std::istringstream is{std::string{s}};
    is >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
    if (is.fail())
        return std::nullopt;
    tm.tm_isdst = -1;
    const std::time_t tt = ::mktime(&tm);
    if (tt == static_cast<std::time_t>(-1))
        return std::nullopt;
    return std::chrono::system_clock::from_time_t(tt);
}

// Serialize to the plain "YYYY-MM-DD HH:MM:SS" custom-field form
// (space separator, no 'T', no 'Z'). LOCAL wall-clock via
// localtime_r — the same basis as the callLength breadcrumb, so the two
// stored representations of an instant agree (FINDING 4). The machine's TZ
// (set per-deployment via the environment) governs; nothing is hardcoded.
std::string formatCustomFieldTimestamp(aid::Timestamp t) {
    const std::time_t tt = std::chrono::system_clock::to_time_t(t);
    std::tm tm{};
    ::localtime_r(&tt, &tm);
    std::ostringstream os;
    os << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return os.str();
}

std::vector<aid::CallId> splitCommaSeparated(std::string_view s) {
    std::vector<aid::CallId> out;
    std::size_t start = 0;
    while (start <= s.size()) {
        const auto comma = s.find(',', start);
        const auto end = comma == std::string_view::npos ? s.size() : comma;
        if (end > start) {
            // Trim leading/trailing spaces.
            auto lo = start;
            auto hi = end;
            while (lo < hi && s[lo] == ' ')
                ++lo;
            while (hi > lo && s[hi - 1] == ' ')
                --hi;
            if (hi > lo) {
                out.push_back(aid::CallId{std::string{s.substr(lo, hi - lo)}});
            }
        }
        if (comma == std::string_view::npos)
            break;
        start = end + 1;
    }
    return out;
}

std::string joinCommaSeparated(const std::vector<aid::CallId>& callIds) {
    std::string out;
    for (std::size_t i = 0; i < callIds.size(); ++i) {
        if (i > 0)
            out.push_back(',');
        out.append(callIds[i].v);
    }
    return out;
}

// Split a ", "-separated CSV of handler logins. Same trim-and-skip-empty
// discipline as splitCommaSeparated (CallId): tolerant of surrounding
// whitespace and stray/empty entries, so a hand-edited customField7 value
// like "alice, bob ,, carol" round-trips to three logins.
std::vector<aid::UserHandle> splitLogins(std::string_view s) {
    std::vector<aid::UserHandle> out;
    std::size_t start = 0;
    while (start <= s.size()) {
        const auto comma = s.find(',', start);
        const auto end = comma == std::string_view::npos ? s.size() : comma;
        if (end > start) {
            auto lo = start;
            auto hi = end;
            while (lo < hi && s[lo] == ' ')
                ++lo;
            while (hi > lo && s[hi - 1] == ' ')
                --hi;
            if (hi > lo) {
                out.push_back(aid::UserHandle{std::string{s.substr(lo, hi - lo)}});
            }
        }
        if (comma == std::string_view::npos)
            break;
        start = end + 1;
    }
    return out;
}

// Join handler logins as a ", "-separated CSV (the format the later
// merge/relevance phases expect when reading customField7 back).
std::string joinLoginsCommaSpace(const std::vector<aid::UserHandle>& handlers) {
    std::string out;
    for (std::size_t i = 0; i < handlers.size(); ++i) {
        if (i > 0)
            out.append(", ");
        out.append(handlers[i].v);
    }
    return out;
}

// Read a string-valued top-level field. Returns nullopt when the key is
// absent OR when the value is JSON-null (OpenProject serializes empty
// custom fields as null). Wrong type returns nullopt too — the caller can
// treat that as "no value" and the test for malformed HAL elsewhere will
// catch genuine corruption.
std::optional<std::string> readOptString(const nlohmann::json& j, const std::string& key) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null() || !it->is_string())
        return std::nullopt;
    return it->get<std::string>();
}

} // namespace

Result<aid::Ticket> parseFromHal(const nlohmann::json& hal, const CustomFieldMap& fields,
                                 const OpStatusMap& statusMap) {
    if (!hal.is_object()) {
        return unexpected(makeInvalid("HAL: top-level is not an object"));
    }

    aid::Ticket out;

    // id — OpenProject returns it as a JSON integer at the top level.
    if (auto it = hal.find("id"); it != hal.end()) {
        if (it->is_number_integer()) {
            out.id.v = std::to_string(it->get<long long>());
        } else if (it->is_string()) {
            out.id.v = it->get<std::string>();
        } else {
            return unexpected(makeInvalid("HAL: id is missing or has unexpected type"));
        }
    } else {
        return unexpected(makeInvalid("HAL: id missing"));
    }

    // subject — required string.
    if (auto s = readOptString(hal, "subject"); s) {
        out.subject = std::move(*s);
    } else {
        return unexpected(makeInvalid("HAL: subject missing or not a string"));
    }

    // description.raw — OpenProject wraps the body in
    // {"format": "...", "raw": "...", "html": "..."}. We only consume raw.
    if (auto it = hal.find("description"); it != hal.end() && it->is_object()) {
        if (auto raw = it->find("raw"); raw != it->end() && raw->is_string()) {
            out.description = raw->get<std::string>();
        }
    }

    // lockVersion — present on every persisted work_package. Required for
    // PATCH; missing here would later cause every save() to 409.
    if (auto it = hal.find("lockVersion"); it != hal.end() && it->is_number_integer()) {
        out.lockVersion = it->get<int>();
    } else {
        return unexpected(makeInvalid("HAL: lockVersion missing or not an integer"));
    }

    // updatedAt — ISO-8601 in UTC.
    if (auto s = readOptString(hal, "updatedAt"); s) {
        if (auto ts = parseIso8601Utc(*s); ts) {
            out.updatedAt = *ts;
        } else {
            return unexpected(makeInvalid("HAL: updatedAt is not a parseable ISO-8601 string"));
        }
    }

    // _links — project (required), status (required), assignee (optional).
    auto linksIt = hal.find("_links");
    if (linksIt == hal.end() || !linksIt->is_object()) {
        return unexpected(makeInvalid("HAL: _links missing"));
    }
    const auto& links = *linksIt;

    if (auto proj = links.find("project"); proj != links.end() && proj->is_object() &&
                                           proj->contains("href") && (*proj)["href"].is_string()) {
        out.projectId = aid::ProjectId{hrefTail((*proj)["href"].get<std::string>())};
    } else {
        return unexpected(makeInvalid("HAL: _links.project.href missing"));
    }

    if (auto st = links.find("status");
        st != links.end() && st->is_object() && st->contains("href") && (*st)["href"].is_string()) {
        const auto sid = aid::StatusId{hrefTail((*st)["href"].get<std::string>())};
        // Carry the raw id verbatim (the UI's statusId contract field) before
        // collapsing it into the 5-value enum — statusFor() is lossy for any
        // status not in the configured set.
        out.statusId = sid;
        out.status = statusMap.statusFor(sid);
    } else {
        return unexpected(makeInvalid("HAL: _links.status.href missing"));
    }

    if (auto a = links.find("assignee");
        a != links.end() && a->is_object() && a->contains("href") && (*a)["href"].is_string()) {
        // UserHandle.v is the OpenProject *login*;
        // OpUserRepo::hrefFor() is keyed by login. The preferred source
        // is _embedded.assignee.login, which is present when the caller
        // fetched with ?include=assignee.
        //
        // If only _links.assignee.href is available we fall back to its
        // tail (the numeric user id) — a deliberate type-narrowing: the
        // resulting UserHandle{<numeric-id>} will NOT round-trip through
        // OpUserRepo::hrefFor (which filters on `login`). Callers that
        // need the login form (assignee comparisons, hrefFor lookups)
        // must therefore either set ?include=assignee on the fetch, or
        // accept that the assignee is "best-effort" until the next read
        // that does include the embedded user. Documented here rather
        // than at every caller because parseFromHal is the funnel.
        std::string handle;
        // _embedded lives at the top level of the HAL document, NOT
        // under _links. We deliberately reach back up to `hal` rather
        // than `links` to fetch the embedded login.
        if (auto emb = hal.find("_embedded"); emb != hal.end() && emb->is_object()) {
            if (auto ea = emb->find("assignee"); ea != emb->end() && ea->is_object()) {
                if (auto lg = ea->find("login"); lg != ea->end() && lg->is_string()) {
                    handle = lg->get<std::string>();
                }
            }
        }
        // No embedded login (fetch didn't ?include=assignee). Prefer the
        // human-readable display name carried on the link title, so the
        // dashboard shows "Alice", not the bare numeric user id "9".
        if (handle.empty()) {
            if (auto ti = a->find("title"); ti != a->end() && ti->is_string()) {
                handle = ti->get<std::string>();
            }
        }
        if (handle.empty()) {
            handle = hrefTail((*a)["href"].get<std::string>());
        }
        if (!handle.empty()) {
            out.assignee = aid::UserHandle{std::move(handle)};
        }
    }

    // Custom fields. Each one is keyed by "customField<numericId>" at the
    // top level. Empty / null is treated as "no value" without erroring
    // out — operator may legitimately leave a field empty.
    if (auto s = readOptString(hal, customFieldName(fields.callId)); s && !s->empty()) {
        out.callIds = splitCommaSeparated(*s);
    }
    if (auto s = readOptString(hal, customFieldName(fields.callerNumber)); s) {
        out.callerNumber = aid::PhoneNumber{std::move(*s)};
    }
    if (auto s = readOptString(hal, customFieldName(fields.calledNumber)); s && !s->empty()) {
        out.calledNumber = aid::PhoneNumber{std::move(*s)};
    }
    if (auto s = readOptString(hal, customFieldName(fields.callStart)); s && !s->empty()) {
        if (auto ts = parseCustomFieldTimestamp(*s); ts)
            out.callStart = *ts;
    }
    if (auto s = readOptString(hal, customFieldName(fields.callEnd)); s && !s->empty()) {
        if (auto ts = parseCustomFieldTimestamp(*s); ts)
            out.callEnd = *ts;
    }
    // callLength is a Formattable (long-text) custom field — same {format, raw}
    // shape as the top-level description. Holds the appended call-log lines.
    if (auto it = hal.find(customFieldName(fields.callLength));
        it != hal.end() && it->is_object()) {
        if (auto raw = it->find("raw"); raw != it->end() && raw->is_string()) {
            out.callLength = raw->get<std::string>();
        }
    }
    // callHandler — Formattable field, same {format, raw} shape; holds the
    // ", "-separated CSV of handler logins. Split into callHandlers.
    if (auto it = hal.find(customFieldName(fields.callHandler));
        it != hal.end() && it->is_object()) {
        if (auto raw = it->find("raw"); raw != it->end() && raw->is_string()) {
            out.callHandlers = splitLogins(raw->get<std::string>());
        }
    }

    return out;
}

nlohmann::json toCreatePayload(const aid::NewTicket& nt, const CustomFieldMap& fields,
                               const OpStatusMap& statusMap,
                               const aid::crosscutting::TicketSystemConfig& cfg,
                               const std::optional<std::string>& assigneeHref) {
    nlohmann::json body = nlohmann::json::object();
    body["subject"] = nt.subject;

    nlohmann::json links = nlohmann::json::object();
    // Status — adapter is the only owner of TicketStatus → href translation.
    links["status"] = {{"href", "/api/v3/statuses/" + statusMap.hrefIdFor(nt.status).v}};
    // Type — operator supplies the numeric type id as a string in cfg.typeCall.
    links["type"] = {{"href", "/api/v3/types/" + cfg.typeCall}};
    // The assignee link must carry the numeric user href (e.g.
    // "/api/v3/users/9"); OpenProject 422s the login form. The caller
    // (OpTicketRepo::create) resolves nt.assignee's login → href via
    // OpUserRepo::hrefFor and passes it in. When no href is supplied
    // (assignee unset) we emit no link rather than a rejected one.
    if (assigneeHref) {
        links["assignee"] = {{"href", *assigneeHref}};
    }
    body["_links"] = std::move(links);

    // Custom fields — only callId, callerNumber, calledNumber are set on
    // create. callStart / callEnd come later (Accept and Hangup).
    body[customFieldName(fields.callId)] = nt.callId.v;
    body[customFieldName(fields.callerNumber)] = nt.callerNumber.v;
    if (nt.calledNumber) {
        body[customFieldName(fields.calledNumber)] = nt.calledNumber->v;
    }

    return body;
}

nlohmann::json toPatchPayload(const aid::Ticket& t, const CustomFieldMap& fields,
                              const OpStatusMap& statusMap,
                              const std::optional<std::string>& assigneeHref) {
    nlohmann::json body = nlohmann::json::object();
    body["lockVersion"] = t.lockVersion;

    // Status + (optional) assignee live under _links per HAL.
    nlohmann::json links = nlohmann::json::object();
    links["status"] = {{"href", "/api/v3/statuses/" + statusMap.hrefIdFor(t.status).v}};
    // Numeric user href only (see toCreatePayload). OpTicketRepo::save
    // resolves t.assignee's login → href before calling; when none is
    // supplied (assignee unset) the link is simply omitted.
    if (assigneeHref) {
        links["assignee"] = {{"href", *assigneeHref}};
    }
    body["_links"] = std::move(links);

    // Top-level scalar fields the daemon owns. We include subject,
    // description, and the custom fields. Omitting unchanged fields would
    // be more efficient but every read goes through parseFromHal so a
    // round-trip overwrite is harmless and avoids the "is this field
    // dirty" tracking we don't want in Ticket.
    body["subject"] = t.subject;
    body["description"] = {{"format", "markdown"}, {"raw", t.description}};
    body[customFieldName(fields.callId)] = joinCommaSeparated(t.callIds);
    body[customFieldName(fields.callerNumber)] = t.callerNumber.v;
    if (t.calledNumber) {
        body[customFieldName(fields.calledNumber)] = t.calledNumber->v;
    } else {
        body[customFieldName(fields.calledNumber)] = nullptr;
    }
    if (t.callStart) {
        body[customFieldName(fields.callStart)] = formatCustomFieldTimestamp(*t.callStart);
    } else {
        body[customFieldName(fields.callStart)] = nullptr;
    }
    if (t.callEnd) {
        body[customFieldName(fields.callEnd)] = formatCustomFieldTimestamp(*t.callEnd);
    } else {
        body[customFieldName(fields.callEnd)] = nullptr;
    }
    // callLength — Formattable field, same {format, raw} shape as description.
    body[customFieldName(fields.callLength)] = {{"format", "markdown"}, {"raw", t.callLength}};
    // NOTE: callHandler is deliberately NOT written here. It is owned solely by
    // toCallHandlerPatch / OpTicketRepo::addCallHandler, whose refetch→union→patch
    // 409 loop re-merges the CSV against the freshest server value on every retry.
    // Writing t.callHandlers (a snapshot from the read that built `t`) on a status/
    // assignee save would overwrite a handler another mailbox recorded concurrently.

    return body;
}

nlohmann::json toCallHandlerPatch(int lockVersion, const std::vector<aid::UserHandle>& handlers,
                                  const CustomFieldMap& fields) {
    // Minimal partial PATCH: lockVersion (mandatory for optimistic locking) +
    // the single callHandler Formattable field. OpenProject leaves every omitted
    // field intact, so this cannot clobber status/assignee/callLength written by a
    // concurrent save — it only ever advances the handler CSV.
    nlohmann::json body = nlohmann::json::object();
    body["lockVersion"] = lockVersion;
    body[customFieldName(fields.callHandler)] = {{"format", "markdown"},
                                                 {"raw", joinLoginsCommaSpace(handlers)}};
    return body;
}

} // namespace aid::adapters::openproject
