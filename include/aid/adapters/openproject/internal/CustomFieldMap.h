#pragma once

#include "aid/value-types/Ids.h"

// CustomFieldMap — domain field name → OpenProject numeric custom-field id.
//
// OpenProject's HAL responses keys custom fields by numeric id
// ("customField12"), while the daemon thinks in domain names ("callId",
// "callerNumber", …). Main resolves the translation once at startup via
// GET /api/v3/work_packages/schema and passes
// the resolved map to the plugin so every parseFromHal / toCreatePayload
// call can speak both sides.
//
// This type is OpenProject-specific — it lives inside the plugin's
// internal headers, not under value-types/, because no other adapter
// shares OpenProject's custom-field model.

namespace aid::adapters::openproject {

struct CustomFieldMap {
    aid::CustomFieldId callId;
    aid::CustomFieldId callerNumber;
    aid::CustomFieldId calledNumber;
    aid::CustomFieldId callStart;
    aid::CustomFieldId callEnd;
    // Formattable (long-text) field holding the appended call-log lines.
    aid::CustomFieldId callLength;
    // Formattable field holding the ", "-separated CSV of handler logins.
    aid::CustomFieldId callHandler;
};

} // namespace aid::adapters::openproject
