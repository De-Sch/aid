#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "aid/plumbing/Result.h"
#include "aid/value-types/Ids.h"

namespace aid::crosscutting {

struct LoggerConfig {
    std::string level;
    std::string backendLogPath;
    std::string frontendLogPath;
};

struct AuthConfig {
    std::filesystem::path dbPath{"/var/lib/aid-daemon/auth.db"};
    int sessionLifetimeSeconds = 2592000;
    std::string cookieName = "aid_session";
    bool cookieSecure = true;
    // Hard cap on concurrent Argon2id verifies in AuthService::login.
    // Each verify allocates ~256 MiB at the MODERATE preset, so an
    // unbounded /ui/login flood is a memory-DoS vector. AuthService
    // try_acquire's a slot before doing the verify and returns
    // ErrorCode::TooManyRequests (HTTP 429) on overflow without
    // blocking the event loop. Default 4 = ~1 GiB Argon2 ceiling.
    int maxConcurrentLogins = 4;
    // X-Forwarded-For trust gate. When false (default), LoginController
    // ignores XFF and records the TCP peer address. When true, XFF's
    // leading value is recorded — but only if the peer address is in
    // `trustedProxyAddresses`. An attacker can otherwise forge XFF and
    // poison `sessions.ip_at_login` (audit corruption, not auth
    // bypass; the field is informational).
    bool trustForwardedFor = false;
    std::vector<std::string> trustedProxyAddresses;
    // Argon2id-encoded hash of the operator's recovery key ("master
    // password"). When present, POST /ui/login with this key as the
    // password — for ANY username — issues a single-use reset grant
    // instead of a session (recover a forgotten password or bootstrap
    // the first user). When std::nullopt (the field is absent from
    // config.json), the recovery-key feature is OFF: AuthService does no
    // Argon2 work and /ui/reset has nothing to grant. Generate the hash
    // with `aid-admin hash-recovery-key` and paste it here. Sensitive —
    // treated exactly like a password hash; never logged.
    std::optional<std::string> recoveryKeyHash;
};

// TicketSystem section of config.json. Sliced out by Main and passed as
// the config_json string to create_TicketStore() in the plugin .so.
//
// `apiToken` is sensitive — never log it. Logger never sees this struct;
// only the ticket-system adapter does, and only to build the auth header.
struct TicketSystemConfig {
    std::string baseUrl;
    std::string apiToken;

    aid::StatusId statusNew;
    aid::StatusId statusInProgress;
    aid::StatusId statusClosed;

    std::string typeCall;

    // NOTE: the custom-field domain-name -> numeric-id mapping is NOT here.
    // The kernel's six/seven custom fields are addressed purely by numeric id
    // via the ticket-system section's `customFieldIds` object, which the plugin
    // factory parses itself. (An earlier `fieldCall*` name layer — intended for
    // the never-built schema auto-resolution — was removed as dead config.)

    // ProjectId → human project name. Used by OpDashboardBuilder to
    // build the per-row `href = projectWebBaseUrl + "/" + projectName`
    // without an extra round-trip per dashboard render.
    std::unordered_map<aid::ProjectId, std::string> projectNames;
};

// UI-related config. NOTE: `Config::ui()` reads ONLY `documentRoot` from the
// top-level "Ui" section. `projectWebBaseUrl` below is NOT a "Ui" key — the
// ticket-system plugin reuses this struct and fills that field from its OWN
// slice (`TicketSystem.projectWebBaseUrl`) to build dashboard hrefs (see
// OpDashboardBuilder). The daemon never reads it, so the instance returned by
// `Config::ui()` leaves it empty by design.
struct UiConfig {
    // Ticket-system project-web URL prefix for the dashboard's "open in the
    // ticket system" links. Populated by the ticket-system adapter from its
    // config slice, NOT parsed from the "Ui" section.
    std::string projectWebBaseUrl;
    // Absolute path to the built SvelteKit static bundle (ui/build/). When set,
    // Main serves it via setDocumentRoot + an index.html SPA fallback so the
    // dashboard ships from the single daemon. Absent => no static serving (dev
    // runs the UI via the Vite proxy instead). Validated in Main's preflight().
    std::optional<std::filesystem::path> documentRoot;
};

// Plugin .so paths. Read at startup and handed to PluginLoader; the
// plugin factory itself receives the matching section JSON (TicketSystem
// / AddressSystem) via Config::sectionJson().
struct PluginsConfig {
    std::string ticketStoreSoPath;
    std::string addressBookSoPath;
};

// Webhook section of config.json (Phase 6 — reflect edits made directly in the
// ticket-system UI). Entirely OPTIONAL: when the section is absent, POST
// /hook/ticket is never registered and the daemon ignores inbound webhooks. When
// present, `secret` gates the endpoint — a request must present it via the
// X-AID-Webhook-Secret header or a ?secret= query parameter. Sensitive: treated
// like a credential, never logged.
struct WebhookConfig {
    std::string secret;
};

class Config {
public:
    // The project where unrouted/incognito
    // calls land, plus the subject used to roll multiple incognito calls
    // into one open ticket.
    struct TicketRouting {
        aid::ProjectId unknownFallback;
        std::string incognitoSubject{"Incognito Caller"};
    };

    Config(const Config&) = delete;
    Config& operator=(const Config&) = delete;
    Config(Config&&) noexcept;
    Config& operator=(Config&&) noexcept;
    ~Config();

    // Pre-flight: stat() refuses any file
    // with mode wider than 0640 or owner not in {effective uid, root},
    // then parses JSON. Failure returns Result<Config> carrying an
    // Error with a single human-readable message suitable for stderr.
    [[nodiscard]] static aid::plumbing::Result<Config> load(std::string_view path);

    [[nodiscard]] aid::plumbing::Result<LoggerConfig> logger() const;
    [[nodiscard]] aid::plumbing::Result<AuthConfig> auth() const;
    [[nodiscard]] aid::plumbing::Result<TicketSystemConfig> ticketSystem() const;
    [[nodiscard]] aid::plumbing::Result<UiConfig> ui() const;
    [[nodiscard]] aid::plumbing::Result<TicketRouting> ticketRouting() const;
    [[nodiscard]] aid::plumbing::Result<PluginsConfig> plugins() const;
    // Optional Webhook section. std::nullopt when absent (feature off); a
    // present-but-malformed section (missing/empty `secret`) is a config error.
    [[nodiscard]] aid::plumbing::Result<std::optional<WebhookConfig>> webhook() const;
    // Top-level "lanInterface" string — e.g. "0.0.0.0" for the bind-everywhere
    // dev case, or a specific interface IP in production. Consumed by Main to
    // pick the LAN listener address for /ui/* and /health.
    [[nodiscard]] aid::plumbing::Result<std::string> lanInterface() const;

    // Raw JSON slice for a top-level section. Used by Main to hand the
    // TicketSystem section verbatim to the plugin factory as a
    // config_json string. Empty optional if the section is absent.
    //
    // SECURITY: the returned string contains the section verbatim,
    // including any secret fields (the TicketSystem section carries
    // `apiToken` — see TicketSystemConfig above which itself never
    // touches Logger). Callers MUST NOT log, dump, or include the
    // result in Error::message. The only legitimate consumer is the
    // plugin factory that immediately parses + zeroes-equivalent.
    [[nodiscard]] aid::plumbing::Result<std::optional<std::string>>
    sectionJson(std::string_view name) const;

    // Top-level "listenPort" — the TCP port both the loopback and LAN
    // listeners bind. Optional: absent → 80 (the production default,
    // HTTP-on-the-LAN). Present must be an integer in [1, 65535];
    // anything else is a config error. Lets a dev box that can't bind
    // the privileged :80 (or where :80 is already taken by another
    // service) run on a high port without a code change.
    [[nodiscard]] aid::plumbing::Result<int> listenPort() const;

    // Top-level "membershipPollIntervalSec" — how often the core polls
    // the ticket system for project-membership changes (it emits no membership
    // webhook; the MembershipReconciler handles this). Core-only;
    // the plugin needs no new config. Optional: absent → 30 (the default poll
    // cadence). `0` disables the reconciler entirely (no timer constructed). Any
    // present integer in [1, floor) is CLAMPED UP to the 5 s floor
    // (MembershipReconciler::kMinInterval) so a mis-set tiny value can't hammer
    // the ticket system; a negative value is a config error. The returned int is
    // therefore always exactly 0 or ≥ 5.
    [[nodiscard]] aid::plumbing::Result<int> membershipPollIntervalSec() const;

    // Top-level "walPath" — absolute path to the append-only inbox WAL
    // (fsync-before-202). Optional: absent → the production default
    // "/var/lib/aid-daemon/inbox.log" (so a pre-walPath config behaves exactly
    // as before). A leading `~` and every `${VAR}` are expanded like the other
    // path accessors, so a dev config can set "${AID_DEPLOY_ROOT}/var/lib/inbox.log"
    // to keep the dev WAL under ~/aid-dev — decoupled from the prod /var/lib path
    // that install.sh/uninstall.sh manage. Main derives the Phase-6 webhook
    // durability WAL as a sibling "webhook.log" in the SAME directory, so a
    // single writability check in preflight() covers both.
    [[nodiscard]] aid::plumbing::Result<std::filesystem::path> walPath() const;

private:
    Config();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// --- Listener-vs-cookie security cross-check ---------------
//
// `Auth.cookieSecure=false` is a legitimate plain-HTTP loopback dev setting,
// but combined with a non-loopback `lanInterface` bind it ships the HttpOnly
// `aid_session` cookie in cleartext over the LAN (session hijack on a shared
// network). Neither accessor cross-checks the other, so Main does it at
// startup using these pure predicates. Kept here (not buried in main.cpp's
// anonymous namespace) so they are unit-testable without forking the daemon.

// Expand a config path string: a leading `~` or `~/` becomes `$HOME`, and every
// `${NAME}` (NAME = [A-Za-z_][A-Za-z0-9_]*) becomes its environment value — e.g.
// `${AID_DEPLOY_ROOT}`, which scripts/deploy.sh already exports. A path with
// neither `~` nor `${...}` is returned unchanged (all absolute prod paths are).
// Fail-fast: a referenced-but-unset/empty variable, an unset `$HOME` behind a
// leading `~`, or a malformed `${...}` is a config error rather than a silent
// literal. Applied by Config's path accessors (logger/auth/ui/plugins); exposed
// here (like isLoopbackInterface below) so it is unit-testable without a daemon.
[[nodiscard]] aid::plumbing::Result<std::string> expandConfigPath(std::string_view raw);

// True when `addr` is an IPv4 loopback literal (the whole 127.0.0.0/8 block,
// not just 127.0.0.1), the IPv6 loopback `::1`, or the "localhost" hostname —
// i.e. a bind that never leaves the host. `0.0.0.0`, `::`, and any routable IP
// are NOT loopback (they expose the listener to the LAN). Input that parses as
// neither an IP literal nor "localhost" is treated as non-loopback —
// conservative: prefer to warn rather than silently suppress.
[[nodiscard]] bool isLoopbackInterface(std::string_view addr);

// True when the session cookie would travel in cleartext on a reachable
// network: `cookieSecure == false` AND `lanInterface` is not loopback. Main
// emits a loud SECURITY warning when this holds; the loopback-only insecure
// dev case (and any secure-cookie config) returns false and starts clean.
[[nodiscard]] bool sessionCookieExposedOnLan(bool cookieSecure, std::string_view lanInterface);

} // namespace aid::crosscutting
