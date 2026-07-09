#pragma once

#include <string>

#include "aid/value-types/Ids.h"

namespace aid {

// Render an instant as ISO-8601 UTC "YYYY-MM-DDTHH:MM:SSZ" (the trailing `Z`
// = Zulu = UTC, zero offset). Uses gmtime_r, so the output is INDEPENDENT of
// the machine timezone — the same instant renders identically everywhere.
//
// This is deliberately NOT the daemon's local wall-clock format. Local
// timestamps (the dashboard's callStart/callEnd, the callHandler breadcrumb)
// are rendered with localtime_r at "YYYY-MM-DD HH:MM:SS" and MUST stay local
// and machine-TZ-bound — do not route those through this helper. This one is
// only for UTC audit/ordering fields (dashboard `updatedAt`, aid-admin session
// listings) where a fixed, zone-free string is wanted.
[[nodiscard]] std::string formatIso8601Utc(Timestamp t);

} // namespace aid
