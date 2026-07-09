# AID2.0 — Core API & Integration Guide

AID2.0 is a long-running C++20 daemon, built on [Drogon](https://github.com/drogonframework/drogon),
that turns phone-system events into ticket-tracker work packages. A call comes in,
your PBX POSTs a small JSON event to `POST /call`, and the daemon takes it from
there: it looks the caller up in an address book, works out which project the call
belongs to, and creates or updates the matching ticket. Operators watch all of this
happen live on a bundled dashboard (REST + WebSocket under `/ui/*`).

The important part is that the ticket tracker and the address book aren't
hard-wired. Each one is a `dlopen`'d plugin sitting behind an abstract *port*, so
[OpenProject](https://www.openproject.org/) (tickets) and DaviCal/CardDAV (contacts)
are simply the two backends that happen to ship in the box. Write your own `.so`
that implements the `TicketStore` or `AddressBook` port and the same daemon will
drive a different tracker — Jira, a CRM, some in-house system — or a different
contact source, without a single change to the core. That plugin seam is the reason
AID2.0 is an *integration* daemon and not just an OpenProject front-end.

The daemon runs with no database of its own. Call state lives in the ticket
backend, contacts live in the address book. The one exception is a small local
SQLite `auth.db`, which only holds dashboard logins.

> Want the big picture first — the layered "plugin-safe core" against the Drogon
> zone, the ports seam, the end-to-end call flow? Start with
> [Architecture](01-architecture.md).

## Who this guide is for

You're here to connect your own phone system to the AID2.0 core, or to extend it to
a different ticket tracker or address book. This guide covers the integration
surface: the HTTP API your PBX drives, and the plugin ABI you implement when you
want to swap a backend.

> **Just running or using AID2.0, not integrating with it?** The
> [User Guide](user-guide/README.md) is what you want — installing the daemon,
> signing in, and working the operator dashboard.

There are two seams to integrate against:

1. **The HTTP API.** Your phone system POSTs call events to `/call`. This is the
   usual case — point your PBX at the daemon and you're done.
2. **The port plugins.** The OpenProject and DaviCal backends are `dlopen`'d `.so`
   files behind abstract *ports*. Implement the `TicketStore` or `AddressBook` port
   and you can bind a different ticket tracker or contact source without touching
   the daemon.

## Table of contents

**New here?** Head to [Getting started](10-getting-started.md) first — run the
daemon, send it a call — then come back for the concepts. The chapters below are in
suggested reading order; the number links to the file.

| # | Chapter | What it covers |
|---|---------|----------------|
| 1 | [Architecture](01-architecture.md) | Layers, the "plugin-safe core" vs the Drogon zone, end-to-end data flow |
| 2 | [Integrating the Call API](02-integrating-call-api.md) | `POST /call` — the five event shapes, request/response, status codes, ordering |
| 3 | [Integrating the Address Book](03-integrating-the-address-book.md) | How your CardDAV books must be set up — the two books, how to store numbers / names / project ids, the lookup |
| 4 | [Webhook & Health](04-webhook-and-health.md) | `POST /hook/ticket` (live edit reflection) and `GET /health` |
| 5 | [Writing a plugin](05-writing-a-plugin.md) | The port ABI: factory symbols, ABI/contract tags, the interfaces to implement, reducer/retry, testing, CMake |
| 6 | [Value types](06-value-types.md) | `Ticket`, `Contact`, `CallEvent`, `Error`, dashboard types, and the ticket state machine |
| 7 | [Configuration](07-configuration.md) | The config JSON schema, every section and default |
| 8 | [Operational model](08-operational-model.md) | WAL durability, the per-callid mailbox, caps, graceful shutdown |
| 9 | [How calls become tickets](09-how-calls-become-tickets.md) | **The behaviour model** — routing, which events create vs update, the ticket lifecycle |
| 10 | [Getting started](10-getting-started.md) | Build, run, send your first call, and a full worked call trace |
| 11 | [Building, testing & scripts](11-building-testing-and-scripts.md) | Build the daemon, run the test suite, and the helper scripts |
| 12 | [Troubleshooting & glossary](12-troubleshooting-and-glossary.md) | Common "why didn't it work" cases, reading the signals, and a glossary |

## Quick facts

| | |
|---|---|
| **Language / runtime** | C++20, Drogon (one process, event-loop driven) |
| **Inbound API** | `POST /call`, `POST /hook/ticket` (optional), `GET /health`, `/ui/*` (dashboard) |
| **Wire format authority** | [Integrating the Call API](02-integrating-call-api.md) — the definition of the five `/call` event shapes |
| **Ticket backend** | `aid_openproject_plugin.so` (implements the `TicketStore` port) |
| **Address backend** | `aid_davical_plugin.so` (implements the `AddressBook` port) |
| **Durability** | Append-only WAL, `fdatasync` before the `202` |
| **Ordering** | Same `callid` strictly in order; different callids in parallel |

## A note on the dashboard API

You'll notice this guide doesn't really document the `/ui/*` REST + WebSocket
surface. That's deliberate: that API exists for building an *alternate frontend*,
which is a different job from wiring up a phone system, and the bundled SvelteKit
dashboard already speaks it. If you do need it, the controllers are `UiController`
and `UiStreamController` under `lib/controllers/`, both behind `SessionGuard`.
