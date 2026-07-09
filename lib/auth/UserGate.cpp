#include "aid/auth/UserGate.h"

#include "aid/auth/UserRepo.h"

namespace aid::auth {

bool userKnown(const UserRepo& repo, const std::optional<aid::UserHandle>& handle) {
    if (!handle) {
        return true; // event carries no user -> not gated
    }
    const auto res = repo.lookupByUsername(handle->v);
    // fail-closed: a DB error is a precondition we cannot confirm -> drop.
    return res && res->has_value();
}

bool userKnown(const UserRepo& repo, const aid::UserHandle& handle) {
    return userKnown(repo, std::optional<aid::UserHandle>{handle});
}

} // namespace aid::auth
