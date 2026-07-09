#pragma once

// DcVCardParser — CardDAV multistatus XML → Contact projection. See
// plan/classes/adapters/DcVCardParser.md and BACKEND_LOGIC.md §6.2.
//
// Pure. No I/O. Static-only by design — a stateless XML projector. The
// XML library used internally is libxml2 with all four XXE-relevant
// parse options off (XML_PARSE_NONET, no DTDLOAD, no NOENT, no DTDATTR);
// see the .cpp for the hardening boilerplate. Malformed input never
// throws — bounds are guarded; a vCard that doesn't parse is skipped
// and the others are returned.
//
// Contact.kind is intentionally NOT set here; DaviCalAdapter stamps
// Person or Company based on which CardDAV book the parse came from.

#include <optional>
#include <string_view>
#include <vector>

#include "aid/value-types/Contact.h"
#include "aid/value-types/Ids.h"

namespace aid::adapters::davical::internal {

class DcVCardParser {
public:
    // multistatus → propstat → <C:address-data> → vCard text → Contact.
    // Empty vector on parse failure (intentionally non-throwing —
    // CardDAV failures are treated as no-match upstream).
    [[nodiscard]] static std::vector<aid::Contact> parse(std::string_view xmlMultistatus);

    // Single vCard text → Contact (FN/ORG/TEL/X-CUSTOM1). Returns
    // nullopt on malformed input; never throws. The old microkernel
    // version crashed on a missing trailing newline (line[length()-1]
    // UB at DaviCal.cpp:407) — every find/substr here is bounds-checked.
    [[nodiscard]] static std::optional<aid::Contact> parseOneVCard(std::string_view vCardText);

    // X-CUSTOM1 → comma-split, trimmed ProjectIds (drop empties).
    // vCard TEXT escapes (RFC 6350 §3.4: "\\", "\,", "\;", "\n") are
    // resolved BEFORE the split, so a client that stores the operator's
    // "7, 8" as the escaped single value `7\, 8` still yields {7, 8}
    // rather than the unusable id `7\`.
    [[nodiscard]] static std::vector<aid::ProjectId> splitProjectIds(std::string_view xCustom1);
};

} // namespace aid::adapters::davical::internal
