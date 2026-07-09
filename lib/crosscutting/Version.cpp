#include "aid/version.hpp"

namespace aid {

// AID_VERSION_STRING is injected as a PRIVATE compile definition on
// aid_crosscutting (= CMake PROJECT_VERSION). Formerly the whole reason the
// aid_core placeholder target existed; folded here so there is no standalone
// target holding a single accessor.
std::string_view version() noexcept {
    return AID_VERSION_STRING;
}

} // namespace aid
