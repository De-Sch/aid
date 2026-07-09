#pragma once

#include <string>
#include <vector>

#include "aid/value-types/Ids.h"

namespace aid {

enum class AddressKind { Person, Company };

struct Contact {
    std::string name;
    std::string companyName;
    AddressKind kind = AddressKind::Person;
    std::vector<PhoneNumber> phoneNumbers;
    std::vector<ProjectId> projectIds;
};

} // namespace aid
