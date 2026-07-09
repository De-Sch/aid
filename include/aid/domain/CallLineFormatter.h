#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "aid/value-types/Ids.h"

namespace aid::domain {

// Build, find, rewrite, complete, and parse the per-callid breadcrumb
// lines inside `ticket.callLength`. Used by Accept (build), Transfer
// (rewriteUser), Hangup (complete), and Dashboard (findOpenLineForUser).
//
// Line formats:
//   open      : "{user}: Call start: {YYYY-MM-DD HH:MM:SS} ({callid})"
//   completed : "{user}: Call start: {start} Call End: {end}"
// The "({callid})" lives on the OPEN line only — it is what findLineFor matches
// to locate the line while the call is live, and it is dropped when the line is
// completed (the callid is then dead weight and confuses operators). The
// "Call End:" marker is what flags a line as closed — see findOpenLineForUser /
// findUsersWithOpenCalls. (Call duration was removed.)
class CallLineFormatter {
public:
    static constexpr std::string_view CALL_START_PREFIX = ": Call start: ";
    static constexpr std::string_view CALL_START_PATTERN = ": Call start:";

    struct LineSpan {
        std::size_t begin;
        std::size_t end;
    };
    struct ParsedStart {
        UserHandle user;
        Timestamp start;
    };

    [[nodiscard]] static std::string buildStart(const UserHandle& user, const Timestamp& start,
                                                const CallId& callid);

    [[nodiscard]] static std::optional<LineSpan> findLineFor(std::string_view description,
                                                             const CallId& callid);

    [[nodiscard]] static std::string rewriteUser(std::string_view line, const UserHandle& newUser);

    [[nodiscard]] static std::string complete(std::string_view line, const Timestamp& end);

    [[nodiscard]] static std::optional<ParsedStart> parseStart(std::string_view line);

    [[nodiscard]] static std::optional<CallId> findOpenLineForUser(std::string_view description,
                                                                   const UserHandle& user);

    // Distinct users that currently have an OPEN "Call start:" line (no
    // matching "Call End:") anywhere in `description`, in first-seen order.
    // Dashboard-support companion to findOpenLineForUser: lets a viewer see
    // which OTHER users hold a live call on a ticket. Reuses parseStart and
    // the same fixed line format; the shared parsing bodies are untouched.
    [[nodiscard]] static std::vector<UserHandle>
    findUsersWithOpenCalls(std::string_view description);

    [[nodiscard]] static bool hasLine(std::string_view description, const UserHandle& user,
                                      const CallId& callid);
};

} // namespace aid::domain
