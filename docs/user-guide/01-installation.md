# 1. Installation

[Back to User Guide](README.md)

The installer is an interactive wizard. You run one command, answer a handful of
questions, and it builds AID2.0 from the checkout and installs it as a background
service under systemd. Once that's done, the checkout is disposable.

## 1.1 Before you start

Have these to hand:

- A Debian-style Linux server with `sudo` access.
- The AID2.0 source checkout on that server (the installer builds it right on the
  machine, so the daemon and its plugins are always one matched build).
- Your OpenProject instance reachable over the network, with:
  - a project for calls, its statuses, and the custom fields AID uses, and
  - an API token plus a user login you'll use to sign in to the dashboard.
- Your CardDAV / DaviCal address-book URLs and credentials.
- A webhook secret (any strong string) if you want OpenProject edits to reflect live
  on the dashboard.

During setup you'll also choose a recovery key — a master password that can reset
any dashboard account. Pick a strong one and keep it somewhere safe; you'll want it
the first time someone forgets their password (see
[Signing in & passwords](02-signing-in.md)).

## 1.2 Run the installer

From the checkout, run:

```sh
sudo ./scripts/install.sh
```

First it installs the build dependencies and compiles the daemon, the admin tool,
and both backend plugins together; then, on a fresh machine, it launches the setup
wizard.

> Want the service to run under an existing account? Use
> `sudo ./scripts/install.sh --run-as <username>`.

## 1.3 Follow the wizard

The wizard asks one question at a time, and each comes with a sensible default in
brackets that you can accept by pressing Enter. In order, it asks for:

| Prompt | What to enter |
|---|---|
| **Port for AID** | the port the daemon listens on (default `8088`) |
| **Membership poll interval** | how often it re-checks OpenProject project membership (default `30`s; `0` disables) |
| **OpenProject objects created?** | confirm you've created the project/statuses/fields it lists |
| **Webhook secret** | your shared secret (or paste the whole OpenProject payload URL) |
| **OpenProject API token** | the API token (kept secret, never logged) |
| **DaviCal URLs / user / password** | your address-book addresses & companies books and login |
| **Incognito caller subject** | the ticket title used for withheld-number calls (default `Incognito Caller`) |
| **Session lifetime / cookie name** | dashboard login session settings (defaults are fine) |
| **Log level** | `INFO` is the sensible default |
| **Recovery key** | your master password for account recovery — **store it safely** |
| **Dashboard username** | the first operator login — **must match an OpenProject login** |
| **Dashboard password** | that operator's password (at least 8 characters) |

From there it auto-discovers the OpenProject status and field IDs for you. If
OpenProject happens to be unreachable at that moment, you can skip discovery and
fill those in later — the service still installs, it just stays parked until you do.

## 1.4 What it installs

The wizard copies everything out to standard system locations and registers a
service:

- the daemon and admin tool in `/usr/local/bin/`,
- data (plugins, dashboard, database, logs) under `/var/lib/aid-daemon/` and
  `/var/log/aid-daemon/`,
- the configuration at `/etc/aid-daemon/config.json`,
- a systemd unit, `aid-daemon.service`.

Since it built everything on the machine, the source checkout isn't needed to run
the service — feel free to delete it later to reclaim the disk space.

## 1.5 After the install — a few manual steps

The installer deliberately leaves your firewall and OpenProject alone. It prints the
exact steps to finish; the short version:

- **Firewall** — let your phone system and your OpenProject host reach the daemon's
  port (for `POST /call` and the webhook), and let the daemon reach OpenProject and
  DaviCal.
- **OpenProject** — point its outgoing webhook at
  `http://<your-host>:<port>/hook/ticket`, passing the webhook secret.

Manage the service with the usual systemd commands:

```sh
systemctl status aid-daemon      # is it running?
systemctl restart aid-daemon     # apply a config change
journalctl -u aid-daemon -f      # follow the logs
```

Once it's up, open `http://<your-host>:<port>/` in a browser and sign in —
[Signing in & passwords](02-signing-in.md).

## 1.6 Upgrading later

To upgrade, pull the new source and re-run `sudo ./scripts/install.sh`. It notices
the existing install, rebuilds, atomically swaps the binaries, plugins, and
dashboard, and restarts — and it leaves your configuration, users, and call data
untouched.

---

Next: [Signing in & passwords →](02-signing-in.md)
