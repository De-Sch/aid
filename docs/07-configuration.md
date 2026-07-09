# 7. Configuration

[← Value types](06-value-types.md) · [Back to index](README.md)

The daemon takes exactly one argument: the path to a JSON config file. This chapter
walks through every section and key the daemon reads (the parsing lives in
`lib/crosscutting/Config.cpp`).

Two of those sections — `TicketSystem` and `AddressSystem` — are passed *verbatim*
to the respective plugin factories as their `config_json`, so they can carry
plugin-specific keys the daemon itself never looks at.

## 7.1 A complete example

```jsonc
{
  // ── top-level ───────────────────────────────────────────────
  "listenPort": 8080,                 // both listeners bind this; absent → 80
  "lanInterface": "0.0.0.0",          // LAN bind address (or a specific IP)
  "walPath": "/var/lib/aid-daemon/inbox.log",   // absent → this same default
  "membershipPollIntervalSec": 30,    // 0 disables; 1..4 clamps up to 5

  "Logger": {
    "level": "info",
    "backendLogPath": "/var/log/aid-daemon/backend.log",
    "frontendLogPath": "/var/log/aid-daemon/frontend.log"
  },

  "Auth": {
    "dbPath": "/var/lib/aid-daemon/auth.db",
    "sessionLifetimeSeconds": 2592000,      // 30 days
    "cookieName": "aid_session",
    "cookieSecure": true,                   // false only for loopback-only HTTP dev
    "maxConcurrentLogins": 4,               // Argon2id memory-DoS cap
    "trustForwardedFor": false,
    "trustedProxyAddresses": [],
    "recoveryKeyHash": null                 // optional; Argon2id hash of a master key
  },

  "TicketSystem": {                         // parsed by daemon AND handed to the plugin
    "baseUrl": "https://openproject.example.com",
    "apiToken": "…",                        // sensitive — never logged
    "typeCall": "Call",
    "statusNew": "1",
    "statusInProgress": "7",
    "statusClosed": "13",
    "projectNames": { "3": "sales", "5": "support" }
    // plugin-side keys (e.g. customFieldIds, projectWebBaseUrl) also live here
  },

  "AddressSystem": {                        // NOT parsed by daemon; handed to the plugin
    "bookAddresses": "http://localhost/davical/caldav.php/aid/addresses/",
    "bookCompanies": "http://localhost/davical/caldav.php/aid/companies/",
    "user": "aid",
    "password": "…",                        // sensitive — never logged
    "defaultRegion": "DE"
  },

  "Plugins": {
    "ticketStore": { "libPath": "/usr/lib/aid-daemon/aid_openproject_plugin.so" },
    "addressBook": { "libPath": "/usr/lib/aid-daemon/aid_davical_plugin.so" }
  },

  "TicketRouting": {
    "unknownFallback": "9",                 // project id for unrouted/incognito calls
    "incognitoSubject": "Incognito Caller"
  },

  "Ui": {
    "documentRoot": "/usr/share/aid-daemon/ui"   // built SvelteKit bundle; omit to disable
  },

  "Webhook": {                              // optional; omit to disable /hook/ticket
    "secret": "…"                           // sensitive — never logged
  }
}
```

> The example uses `//` comments to explain things. The real file is strict JSON, so
> strip the comments out.

## 7.2 Top-level keys

| Key | Type | Default | Notes |
|---|---|---|---|
| `listenPort` | int `[1,65535]` | `80` | both the loopback and LAN listeners bind this port |
| `lanInterface` | string | *(required)* | `"0.0.0.0"` to bind everywhere, or a specific IP; drives the LAN listener for `/ui/*` and `/health` |
| `walPath` | path | `/var/lib/aid-daemon/inbox.log` | append-only WAL; the webhook WAL is a sibling `webhook.log` in the same directory. Supports `~` and `${VAR}` expansion |
| `membershipPollIntervalSec` | int | `30` | project-membership poll cadence. `0` disables the reconciler; `1..4` is clamped up to the 5-second floor; negative is an error |

## 7.3 Sections

| Section | Required keys | Optional keys |
|---|---|---|
| `Logger` | `level`, `backendLogPath`, `frontendLogPath` | — |
| `Auth` | — (all defaulted) | `dbPath`, `sessionLifetimeSeconds`, `cookieName`, `cookieSecure`, `maxConcurrentLogins`, `trustForwardedFor`, `trustedProxyAddresses[]`, `recoveryKeyHash` |
| `TicketSystem` | `baseUrl`, `apiToken`, `typeCall`, `statusNew`, `statusInProgress`, `statusClosed` | `projectNames{}` + any plugin-specific keys |
| `AddressSystem` | *(defined by the address plugin)* | for DaviCal: `bookAddresses`, `bookCompanies`, `defaultRegion` (required by the plugin), `user`, `password` |
| `Plugins` | `ticketStore.libPath`, `addressBook.libPath` | — |
| `TicketRouting` | `unknownFallback` | `incognitoSubject` (default `"Incognito Caller"`) |
| `Ui` | — | `documentRoot` (omit → no static serving) |
| `Webhook` | `secret` (if the section is present) | — omit the whole section to disable |

A few specifics worth calling out:

- **`TicketSystem` is dual-purpose.** The daemon parses the keys above, *and* the
  whole section is passed verbatim to the ticket plugin. Any key the daemon doesn't
  recognize — `customFieldIds` or `projectWebBaseUrl` for the OpenProject plugin,
  for instance — the daemon just ignores and the plugin consumes.
- **`AddressSystem` is plugin-only.** The daemon doesn't parse it at all; it simply
  hands the section to the address plugin. The keys shown here are the ones the
  DaviCal plugin requires.
- **`Auth.cookieSecure` and `lanInterface` are cross-checked.** Set
  `cookieSecure: false` while binding a non-loopback `lanInterface` and you're
  shipping the session cookie in cleartext over the LAN. When that happens the
  daemon prints a loud SECURITY warning at startup — it's a legitimate loopback-only
  dev setting, but a real hazard on an actual LAN.
- **`recoveryKeyHash`** is the Argon2id hash of an operator master key. When it's
  set, logging in with that key as the password (under any username) mints a
  single-use password-reset grant instead of a session — handy for bootstrapping the
  first user. Leave it out and the feature is off. Generate the hash with
  `aid-admin hash-recovery-key`.

## 7.4 Config-file hardening

`Config::load` won't read a config file that's too permissive:

- The file mode has to be no wider than `0640`.
- The owner has to be the daemon's effective uid, or root.
- It's opened with `O_NOFOLLOW`, so it won't follow a symlink.

Break any of those and startup aborts with one clear stderr line. The sensitive
fields — `apiToken`, `AddressSystem.password`, `Webhook.secret`, `recoveryKeyHash` —
never make it into any log.

---

Next: [Operational model →](08-operational-model.md)
