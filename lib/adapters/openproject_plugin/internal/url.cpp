#include "aid/adapters/openproject/internal/url.h"

#include <nlohmann/json.hpp>
#include <string>
#include <utility>

namespace aid::adapters::openproject {

std::string urlEncode(std::string_view s) {
    std::string out;
    out.reserve(s.size() * 3);
    for (char c : s) {
        const auto u = static_cast<unsigned char>(c);
        const bool unreserved = (u >= '0' && u <= '9') || (u >= 'A' && u <= 'Z') ||
                                (u >= 'a' && u <= 'z') || c == '-' || c == '_' || c == '.' ||
                                c == '~';
        if (unreserved) {
            out.push_back(c);
        } else {
            static constexpr char kHex[] = "0123456789ABCDEF";
            out.push_back('%');
            out.push_back(kHex[u >> 4]);
            out.push_back(kHex[u & 0x0F]);
        }
    }
    return out;
}

std::string hrefTail(std::string_view href) {
    if (href.empty()) {
        return {};
    }
    const auto pos = href.rfind('/');
    if (pos == std::string_view::npos) {
        return std::string{href};
    }
    return std::string{href.substr(pos + 1)};
}

std::string customFieldName(const aid::CustomFieldId& id) {
    return "customField" + id.v;
}

std::string singleFilterUrl(std::string_view path, std::string_view field, std::string_view op,
                            std::string_view value) {
    nlohmann::json filter = nlohmann::json::array();
    nlohmann::json one = nlohmann::json::object();
    nlohmann::json clause = nlohmann::json::object();
    clause["operator"] = std::string{op};
    clause["values"] = nlohmann::json::array({std::string{value}});
    one[std::string{field}] = std::move(clause);
    filter.push_back(std::move(one));

    std::string out;
    out.append(path);
    out.append("?filters=");
    out.append(urlEncode(filter.dump()));
    return out;
}

std::string multiFilterUrl(std::string_view path, const nlohmann::json& filters) {
    std::string out;
    out.append(path);
    out.append("?filters=");
    out.append(urlEncode(filters.dump()));
    return out;
}

} // namespace aid::adapters::openproject
