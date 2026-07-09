#include "aid/serialization/ActionResultJson.h"

#include "aid/plumbing/ActionResult.h"

namespace aid::serialization {

nlohmann::json toJson(const aid::plumbing::ActionResult& result) {
    nlohmann::json j;
    j["ok"] = result.ok;
    j["op"] = result.op;
    j["ticketId"] = result.ticketId.v;
    // Emit null (not "") when there is no message, so REST and WS agree.
    if (result.message.has_value()) {
        j["message"] = *result.message;
    } else {
        j["message"] = nullptr;
    }
    return j;
}

} // namespace aid::serialization
