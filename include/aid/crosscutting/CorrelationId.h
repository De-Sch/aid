#pragma once

#include <string>

namespace aid::crosscutting {

class CorrelationId {
public:
    [[nodiscard]] static std::string nextUuid();
};

} // namespace aid::crosscutting
