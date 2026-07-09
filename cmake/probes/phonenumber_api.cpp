// CMake configure-time probe: confirms the system libphonenumber exposes
// the exact three API calls aid::adapters::davical::DaviCalAdapter::
// canonicalize() needs (Parse + IsPossibleNumber + Format). Linked
// against AID_PHONENUMBER_LIB during configure; not part of the daemon.
//
// If this file refuses to link, libphonenumber-dev is missing or too old
// — the FATAL_ERROR in the top-level CMakeLists shows the operator
// `sudo apt install libphonenumber-dev` and the probe build output.

#include <phonenumbers/phonenumberutil.h>

#include <string>

int main() {
    using i18n::phonenumbers::PhoneNumber;
    using i18n::phonenumbers::PhoneNumberUtil;

    const auto& util = *PhoneNumberUtil::GetInstance();
    PhoneNumber n;
    if (util.Parse("0170 1234567", "DE", &n) != PhoneNumberUtil::NO_PARSING_ERROR) {
        return 1;
    }
    if (!util.IsPossibleNumber(n)) {
        return 2;
    }
    std::string out;
    util.Format(n, PhoneNumberUtil::E164, &out);
    return out.empty() ? 3 : 0;
}
