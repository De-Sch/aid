#include "aid/crosscutting/Config.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cassert>
#include <cctype>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aid/plumbing/Error.h"

namespace aid::crosscutting {

using aid::plumbing::Error;
using aid::plumbing::ErrorCode;
using aid::plumbing::Result;
using aid::plumbing::unexpected;

struct Config::Impl {
    nlohmann::json root;
    std::filesystem::path path;
};

Config::Config() = default;
Config::Config(Config&&) noexcept = default;
Config& Config::operator=(Config&&) noexcept = default;
Config::~Config() = default;

namespace {

Error makeError(std::string msg) {
    return Error{ErrorCode::InvalidInput, std::move(msg), std::nullopt};
}

// nlohmann::json field lookup helpers — return std::nullopt when the
// node is missing or has the wrong type. Branching on the result keeps
// the slice accessors readable.
const nlohmann::json* find(const nlohmann::json& j, std::string_view key) {
    auto it = j.find(key);
    return it == j.end() ? nullptr : &*it;
}

Result<std::string> requireString(const nlohmann::json& j, std::string_view section,
                                  std::string_view key) {
    const auto* node = find(j, key);
    if (node == nullptr || !node->is_string()) {
        std::ostringstream msg;
        msg << "config: " << section << "." << key << " missing or not a string";
        return unexpected(makeError(msg.str()));
    }
    return node->get<std::string>();
}

// nlohmann widens to int64 before narrowing to int; do the bounds check
// ourselves so we return a typed Error instead of letting a json::type_error
// escape on overflow.
Result<int> readInt(const nlohmann::json& node, std::string_view section, std::string_view key) {
    if (!node.is_number_integer()) {
        std::ostringstream msg;
        msg << "config: " << section << "." << key << " must be an integer";
        return unexpected(makeError(msg.str()));
    }
    const auto raw = node.get<std::int64_t>();
    if (raw < INT_MIN || raw > INT_MAX) {
        std::ostringstream msg;
        msg << "config: " << section << "." << key << " " << raw << " is out of int range";
        return unexpected(makeError(msg.str()));
    }
    return static_cast<int>(raw);
}

} // namespace

namespace {

// RAII wrapper for a POSIX file descriptor — guarantees ::close()
// regardless of which early-return the load path takes.
class FdGuard {
public:
    explicit FdGuard(int fd) noexcept : fd_(fd) {}
    ~FdGuard() {
        if (fd_ >= 0)
            ::close(fd_);
    }
    FdGuard(const FdGuard&) = delete;
    FdGuard& operator=(const FdGuard&) = delete;
    FdGuard(FdGuard&&) = delete;
    FdGuard& operator=(FdGuard&&) = delete;

    [[nodiscard]] int get() const noexcept { return fd_; }

private:
    int fd_;
};

} // namespace

Result<Config> Config::load(std::string_view path) {
    namespace fs = std::filesystem;
    const std::string pathStr{path};
    const fs::path p{pathStr};

    // O_NOFOLLOW rejects symlinks; O_CLOEXEC keeps the fd out of any
    // child process. fstat() on this fd closes the TOCTOU window —
    // the mode/owner check and the read happen on the same inode.
    const int fd = ::open(pathStr.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ELOOP) {
            return unexpected(
                makeError("config: refusing to load " + pathStr + " — it is a symlink"));
        }
        return unexpected(makeError("config: cannot open " + pathStr));
    }
    FdGuard fdg{fd};

    struct stat st {};
    if (::fstat(fd, &st) != 0) {
        return unexpected(makeError("config: cannot fstat " + pathStr));
    }

    if (!S_ISREG(st.st_mode)) {
        return unexpected(
            makeError("config: refusing to load " + pathStr + " — not a regular file"));
    }

    const auto mode = st.st_mode & 0777;
    // 0027 = group-write | world-rwx. Any of those set means wider
    // than 0640. Owner read/write (0600) and group-read (0040) are
    // allowed; nothing else.
    if ((mode & 0027) != 0) {
        std::ostringstream msg;
        msg << "config: refusing to load " << pathStr << " — mode 0" << std::oct << std::setw(4)
            << std::setfill('0') << mode << " is wider than 0640";
        return unexpected(makeError(msg.str()));
    }

    const auto myUid = ::geteuid();
    if (st.st_uid != myUid && st.st_uid != 0) {
        return unexpected(
            makeError("config: " + pathStr + " owner is not the daemon user or root"));
    }

    // Read the file contents off the fd. Avoids reopening by path,
    // which would re-introduce the TOCTOU.
    std::vector<char> buf;
    constexpr std::size_t kChunk = 4096;
    for (;;) {
        const std::size_t oldSize = buf.size();
        buf.resize(oldSize + kChunk);
        const ssize_t n = ::read(fd, buf.data() + oldSize, kChunk);
        if (n < 0) {
            return unexpected(makeError("config: read failed on " + pathStr));
        }
        buf.resize(oldSize + static_cast<std::size_t>(n));
        if (n == 0)
            break;
    }

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(buf.begin(), buf.end());
    } catch (const nlohmann::json::parse_error& e) {
        return unexpected(makeError(std::string{"config: parse error: "} + e.what()));
    }

    if (!j.is_object()) {
        return unexpected(makeError("config: top-level JSON must be an object"));
    }

    Config c;
    c.impl_ = std::make_unique<Impl>(Impl{std::move(j), p});
    return c;
}

Result<LoggerConfig> Config::logger() const {
    assert(impl_ && "Config::logger() called on a moved-from instance");
    const auto* section = find(impl_->root, "Logger");
    if (section == nullptr || !section->is_object()) {
        return unexpected(makeError("config: Logger section missing or not an object"));
    }

    LoggerConfig out;
    if (auto r = requireString(*section, "Logger", "level"); r) {
        out.level = std::move(*r);
    } else {
        return unexpected(r.error());
    }
    if (auto r = requireString(*section, "Logger", "backendLogPath"); r) {
        auto ex = expandConfigPath(*r);
        if (!ex)
            return unexpected(ex.error());
        out.backendLogPath = std::move(*ex);
    } else {
        return unexpected(r.error());
    }
    if (auto r = requireString(*section, "Logger", "frontendLogPath"); r) {
        auto ex = expandConfigPath(*r);
        if (!ex)
            return unexpected(ex.error());
        out.frontendLogPath = std::move(*ex);
    } else {
        return unexpected(r.error());
    }
    return out;
}

Result<TicketSystemConfig> Config::ticketSystem() const {
    assert(impl_ && "Config::ticketSystem() called on a moved-from instance");
    const auto* section = find(impl_->root, "TicketSystem");
    if (section == nullptr || !section->is_object()) {
        return unexpected(makeError("config: TicketSystem section missing or not an object"));
    }

    TicketSystemConfig out;

    // Required string fields. Each one short-circuits on the first miss
    // so the operator gets a single targeted error per startup attempt.
    struct StringSlot {
        std::string_view key;
        std::string* dest;
    };
    const StringSlot strSlots[] = {
        {"baseUrl", &out.baseUrl},
        {"apiToken", &out.apiToken},
        {"typeCall", &out.typeCall},
    };
    for (const auto& slot : strSlots) {
        auto r = requireString(*section, "TicketSystem", slot.key);
        if (!r)
            return unexpected(r.error());
        *slot.dest = std::move(*r);
    }

    struct StatusSlot {
        std::string_view key;
        aid::StatusId* dest;
    };
    const StatusSlot statusSlots[] = {
        {"statusNew", &out.statusNew},
        {"statusInProgress", &out.statusInProgress},
        {"statusClosed", &out.statusClosed},
    };
    for (const auto& slot : statusSlots) {
        auto r = requireString(*section, "TicketSystem", slot.key);
        if (!r)
            return unexpected(r.error());
        slot.dest->v = std::move(*r);
    }

    // projectNames is an object mapping projectId → name. Required for
    // dashboard href construction; an empty object is accepted (operator
    // may not yet have any projects to surface) but a wrong-shape value
    // is rejected.
    if (const auto* names = find(*section, "projectNames"); names != nullptr) {
        if (!names->is_object()) {
            return unexpected(makeError("config: TicketSystem.projectNames must be an object"));
        }
        for (auto it = names->begin(); it != names->end(); ++it) {
            if (!it.value().is_string()) {
                std::ostringstream msg;
                msg << "config: TicketSystem.projectNames['" << it.key()
                    << "'] must map to a string";
                return unexpected(makeError(msg.str()));
            }
            out.projectNames.emplace(aid::ProjectId{it.key()}, it.value().get<std::string>());
        }
    }

    return out;
}

Result<UiConfig> Config::ui() const {
    assert(impl_ && "Config::ui() called on a moved-from instance");
    const auto* section = find(impl_->root, "Ui");
    if (section == nullptr || !section->is_object()) {
        return unexpected(makeError("config: Ui section missing or not an object"));
    }

    UiConfig out;
    // NOTE: the "Ui" section carries only documentRoot. `out.projectWebBaseUrl`
    // is intentionally left empty here — it is filled by the ticket-system
    // plugin from its own slice (TicketSystem.projectWebBaseUrl), which is the
    // single source the dashboard href builder reads. See UiConfig in Config.h.

    // documentRoot is optional — absent means the daemon serves only the API
    // (dev uses the Vite proxy). Pure parse here; the filesystem check (exists,
    // directory, has index.html, not world-writable) lives in Main::preflight,
    // consistent with how every other dependent path is validated.
    if (const auto* node = find(*section, "documentRoot"); node != nullptr) {
        if (!node->is_string()) {
            return unexpected(makeError("config: Ui.documentRoot must be a string"));
        }
        auto ex = expandConfigPath(node->get<std::string>());
        if (!ex)
            return unexpected(ex.error());
        out.documentRoot = std::filesystem::path{std::move(*ex)};
    }
    return out;
}

Result<std::optional<std::string>> Config::sectionJson(std::string_view name) const {
    assert(impl_ && "Config::sectionJson() called on a moved-from instance");
    const auto* section = find(impl_->root, name);
    if (section == nullptr) {
        return std::optional<std::string>{};
    }
    // dump(0) emits compact JSON; the plugin will re-parse it via the
    // same nlohmann::json inside its .so, so the textual round-trip is
    // safe (no precision loss for the strings/numbers we use here).
    return std::optional<std::string>{section->dump()};
}

Result<Config::TicketRouting> Config::ticketRouting() const {
    assert(impl_ && "Config::ticketRouting() called on a moved-from instance");
    const auto* section = find(impl_->root, "TicketRouting");
    if (section == nullptr || !section->is_object()) {
        return unexpected(makeError("config: TicketRouting section missing or not an object"));
    }

    TicketRouting out;
    if (auto r = requireString(*section, "TicketRouting", "unknownFallback"); r) {
        out.unknownFallback.v = std::move(*r);
    } else {
        return unexpected(r.error());
    }
    // incognitoSubject is optional — the struct default ("Incognito Caller")
    // covers the omitted case.
    if (const auto* node = find(*section, "incognitoSubject"); node != nullptr) {
        if (!node->is_string()) {
            return unexpected(makeError("config: TicketRouting.incognitoSubject must be a string"));
        }
        out.incognitoSubject = node->get<std::string>();
    }
    return out;
}

Result<PluginsConfig> Config::plugins() const {
    assert(impl_ && "Config::plugins() called on a moved-from instance");
    const auto* section = find(impl_->root, "Plugins");
    if (section == nullptr || !section->is_object()) {
        return unexpected(makeError("config: Plugins section missing or not an object"));
    }

    const auto* ts = find(*section, "ticketStore");
    if (ts == nullptr || !ts->is_object()) {
        return unexpected(makeError("config: Plugins.ticketStore missing or not an object"));
    }
    const auto* ab = find(*section, "addressBook");
    if (ab == nullptr || !ab->is_object()) {
        return unexpected(makeError("config: Plugins.addressBook missing or not an object"));
    }

    PluginsConfig out;
    if (auto r = requireString(*ts, "Plugins.ticketStore", "libPath"); r) {
        auto ex = expandConfigPath(*r);
        if (!ex)
            return unexpected(ex.error());
        out.ticketStoreSoPath = std::move(*ex);
    } else {
        return unexpected(r.error());
    }
    if (auto r = requireString(*ab, "Plugins.addressBook", "libPath"); r) {
        auto ex = expandConfigPath(*r);
        if (!ex)
            return unexpected(ex.error());
        out.addressBookSoPath = std::move(*ex);
    } else {
        return unexpected(r.error());
    }
    return out;
}

Result<std::optional<WebhookConfig>> Config::webhook() const {
    assert(impl_ && "Config::webhook() called on a moved-from instance");
    const auto* section = find(impl_->root, "Webhook");
    // Absent => feature off. Not an error.
    if (section == nullptr) {
        return std::optional<WebhookConfig>{};
    }
    if (!section->is_object()) {
        return unexpected(makeError("config: Webhook section must be an object"));
    }
    WebhookConfig out;
    auto r = requireString(*section, "Webhook", "secret");
    if (!r) {
        return unexpected(r.error());
    }
    if (r->empty()) {
        return unexpected(makeError("config: Webhook.secret must be a non-empty string"));
    }
    out.secret = std::move(*r);
    return std::optional<WebhookConfig>{std::move(out)};
}

Result<std::string> Config::lanInterface() const {
    assert(impl_ && "Config::lanInterface() called on a moved-from instance");
    const auto* node = find(impl_->root, "lanInterface");
    if (node == nullptr || !node->is_string()) {
        return unexpected(makeError("config: top-level lanInterface missing or not a string"));
    }
    return node->get<std::string>();
}

Result<int> Config::listenPort() const {
    assert(impl_ && "Config::listenPort() called on a moved-from instance");
    const auto* node = find(impl_->root, "listenPort");
    // Optional: absent → production default 80 (HTTP-on-the-LAN).
    if (node == nullptr) {
        return 80;
    }
    // Reject non-integers (incl. floats and numeric strings) — the listen
    // port is operator-facing config; a silently-truncated "8088.0" or a
    // string "8088" should fail loudly, not bind something unexpected.
    if (!node->is_number_integer()) {
        return unexpected(makeError("config: top-level listenPort must be an integer"));
    }
    const auto port = node->get<std::int64_t>();
    if (port < 1 || port > 65535) {
        return unexpected(makeError("config: top-level listenPort must be in [1, 65535]"));
    }
    return static_cast<int>(port);
}

Result<int> Config::membershipPollIntervalSec() const {
    assert(impl_ && "Config::membershipPollIntervalSec() called on a moved-from instance");
    // Floor duplicated from MembershipReconciler::kMinInterval (5 s): crosscutting
    // must not link infrastructure, so the constant can't be
    // referenced here. Keep the two in sync — the header documents the coupling.
    constexpr int kFloor = 5;
    constexpr int kDefault = 30;

    const auto* node = find(impl_->root, "membershipPollIntervalSec");
    // Optional: absent → default cadence.
    if (node == nullptr) {
        return kDefault;
    }
    if (!node->is_number_integer()) {
        return unexpected(
            makeError("config: top-level membershipPollIntervalSec must be an integer"));
    }
    const auto secs = node->get<std::int64_t>();
    if (secs < 0) {
        return unexpected(
            makeError("config: top-level membershipPollIntervalSec must be >= 0 (0 disables)"));
    }
    // 0 is the explicit "disabled" sentinel — pass it through untouched. Any
    // positive value below the floor is clamped up so a typo'd "1" can't hammer
    // the ticket system every second.
    if (secs == 0) {
        return 0;
    }
    if (secs > INT_MAX) {
        return unexpected(
            makeError("config: top-level membershipPollIntervalSec is out of int range"));
    }
    return secs < kFloor ? kFloor : static_cast<int>(secs);
}

Result<std::filesystem::path> Config::walPath() const {
    assert(impl_ && "Config::walPath() called on a moved-from instance");
    const auto* node = find(impl_->root, "walPath");
    // Optional: absent → the production default (matches AuthConfig::dbPath's
    // default, so an older config with no walPath keeps its historical path).
    if (node == nullptr) {
        return std::filesystem::path{"/var/lib/aid-daemon/inbox.log"};
    }
    if (!node->is_string()) {
        return unexpected(makeError("config: top-level walPath must be a string"));
    }
    auto ex = expandConfigPath(node->get<std::string>());
    if (!ex)
        return unexpected(ex.error());
    return std::filesystem::path{std::move(*ex)};
}

Result<AuthConfig> Config::auth() const {
    assert(impl_ && "Config::auth() called on a moved-from instance");
    AuthConfig out; // defaults match the documented config spec.

    const auto* section = find(impl_->root, "Auth");
    if (section == nullptr) {
        return out; // entire section absent — use struct defaults.
    }
    if (!section->is_object()) {
        return unexpected(makeError("config: Auth section is not an object"));
    }

    if (const auto* node = find(*section, "dbPath"); node != nullptr) {
        if (!node->is_string()) {
            return unexpected(makeError("config: Auth.dbPath must be a string"));
        }
        auto ex = expandConfigPath(node->get<std::string>());
        if (!ex)
            return unexpected(ex.error());
        out.dbPath = std::move(*ex);
    }
    if (const auto* node = find(*section, "sessionLifetimeSeconds"); node != nullptr) {
        auto v = readInt(*node, "Auth", "sessionLifetimeSeconds");
        if (!v)
            return unexpected(v.error());
        out.sessionLifetimeSeconds = *v;
    }
    if (const auto* node = find(*section, "cookieName"); node != nullptr) {
        if (!node->is_string()) {
            return unexpected(makeError("config: Auth.cookieName must be a string"));
        }
        out.cookieName = node->get<std::string>();
    }
    if (const auto* node = find(*section, "cookieSecure"); node != nullptr) {
        if (!node->is_boolean()) {
            return unexpected(makeError("config: Auth.cookieSecure must be a boolean"));
        }
        out.cookieSecure = node->get<bool>();
    }
    if (const auto* node = find(*section, "maxConcurrentLogins"); node != nullptr) {
        auto v = readInt(*node, "Auth", "maxConcurrentLogins");
        if (!v)
            return unexpected(v.error());
        out.maxConcurrentLogins = *v;
    }
    if (const auto* node = find(*section, "trustForwardedFor"); node != nullptr) {
        if (!node->is_boolean()) {
            return unexpected(makeError("config: Auth.trustForwardedFor must be a boolean"));
        }
        out.trustForwardedFor = node->get<bool>();
    }
    if (const auto* node = find(*section, "trustedProxyAddresses"); node != nullptr) {
        if (!node->is_array()) {
            return unexpected(
                makeError("config: Auth.trustedProxyAddresses must be an array of strings"));
        }
        for (const auto& el : *node) {
            if (!el.is_string()) {
                return unexpected(
                    makeError("config: Auth.trustedProxyAddresses entries must be strings"));
            }
            out.trustedProxyAddresses.push_back(el.get<std::string>());
        }
    }
    if (const auto* node = find(*section, "recoveryKeyHash"); node != nullptr) {
        if (!node->is_string()) {
            return unexpected(makeError("config: Auth.recoveryKeyHash must be a string"));
        }
        auto h = node->get<std::string>();
        // Fail loud on an empty string: PasswordHasher::verify would
        // return false for everything anyway, so an empty hash silently
        // disables the feature an operator believes they just enabled.
        if (h.empty()) {
            return unexpected(makeError("config: Auth.recoveryKeyHash must not be empty"));
        }
        out.recoveryKeyHash = std::move(h);
    }
    // Operator footgun: trustForwardedFor=true with no allowlist is
    // indistinguishable from leaving the flag off (the std::find would
    // never hit). Reject at load so a half-configured proxy gate fails
    // loudly instead of silently dropping XFF audit data.
    if (out.trustForwardedFor && out.trustedProxyAddresses.empty()) {
        return unexpected(makeError("config: Auth.trustForwardedFor=true requires at least one "
                                    "entry in Auth.trustedProxyAddresses"));
    }

    return out;
}

Result<std::string> expandConfigPath(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());

    // Leading ~ or ~/ -> $HOME. A bare ~ mid-path is NOT special (only the
    // shell's login-name form, which we deliberately don't support).
    std::size_t i = 0;
    if (!raw.empty() && raw[0] == '~' && (raw.size() == 1 || raw[1] == '/')) {
        const char* home = std::getenv("HOME");
        if (home == nullptr || home[0] == '\0') {
            return unexpected(
                makeError("config: cannot expand leading '~' — $HOME is unset or empty"));
        }
        out += home;
        i = 1; // skip the '~'; the following '/' (if any) is copied below
    }

    // ${NAME} -> environment value. NAME is [A-Za-z_][A-Za-z0-9_]*.
    for (; i < raw.size(); ++i) {
        if (raw[i] != '$' || i + 1 >= raw.size() || raw[i + 1] != '{') {
            out += raw[i];
            continue;
        }
        const std::size_t close = raw.find('}', i + 2);
        if (close == std::string_view::npos) {
            return unexpected(makeError(
                std::string{"config: malformed variable reference (missing '}') in path: "} +
                std::string{raw}));
        }
        const std::string_view name = raw.substr(i + 2, close - (i + 2));
        const bool validName =
            !name.empty() &&
            (std::isalpha(static_cast<unsigned char>(name[0])) || name[0] == '_') &&
            std::all_of(name.begin(), name.end(),
                        [](unsigned char c) { return std::isalnum(c) || c == '_'; });
        if (!validName) {
            return unexpected(makeError(std::string{"config: invalid variable name '${"} +
                                        std::string{name} + "}' in path"));
        }
        const char* val = std::getenv(std::string{name}.c_str());
        if (val == nullptr || val[0] == '\0') {
            return unexpected(makeError(std::string{"config: unset environment variable '${"} +
                                        std::string{name} + "}' referenced in path"));
        }
        out += val;
        i = close; // loop's ++i steps past '}'
    }

    return out;
}

bool isLoopbackInterface(std::string_view addr) {
    // "localhost" never resolves off-host in practice; accept it explicitly so
    // a dev who binds the hostname rather than the literal isn't warned.
    if (addr == "localhost") {
        return true;
    }
    // inet_pton needs a NUL-terminated C string; string_view may not be one.
    const std::string s{addr};

    // IPv6 first: ::1 is the only loopback address. Compare against the libc
    // constant rather than string-matching (handles "::1", "0:0:...:1", etc.).
    in6_addr v6{};
    if (::inet_pton(AF_INET6, s.c_str(), &v6) == 1) {
        return std::memcmp(&v6, &in6addr_loopback, sizeof(v6)) == 0;
    }
    // IPv4: the entire 127.0.0.0/8 block is loopback, not just 127.0.0.1.
    in_addr v4{};
    if (::inet_pton(AF_INET, s.c_str(), &v4) == 1) {
        return (::ntohl(v4.s_addr) & 0xff000000U) == 0x7f000000U;
    }
    // Not a parseable IP literal and not "localhost" (e.g. "0.0.0.0", "::", a
    // routable IP, or garbage) — treat as non-loopback so the cleartext-cookie
    // warning fires rather than being silently suppressed.
    return false;
}

bool sessionCookieExposedOnLan(bool cookieSecure, std::string_view lanInterface) {
    return !cookieSecure && !isLoopbackInterface(lanInterface);
}

} // namespace aid::crosscutting
