# 10. Getting started

[← Architecture](01-architecture.md) · [Back to index](README.md)

This chapter takes you from a fresh clone to a call producing a ticket, and walks
one full call end to end so you can watch the behaviour from
[Chapter 9](09-how-calls-become-tickets.md) play out for real.

## 10.1 What you need

The daemon is a bridge, so it needs the two backends it bridges between:

- **A ticket system** reachable over HTTP — OpenProject, for the shipped
  `aid_openproject_plugin.so` (you'll need an API token plus a project, statuses,
  and the custom fields it uses).
- **A CardDAV address book** — DaviCal, for the shipped `aid_davical_plugin.so` (two
  address books: addresses and companies). This doubles as your routing table: a
  contact's assigned projects decide where its calls land
  ([§9.1](09-how-calls-become-tickets.md)).
- A toolchain to build the daemon (see the root `README.md` — `./scripts/build.sh`).

Binding a *different* ticket tracker or address source? Then you implement a plugin
instead (see [Writing a plugin](05-writing-a-plugin.md)) — but the run-and-verify
loop below stays the same.

## 10.2 Build and run

```sh
./scripts/build.sh            # → daemon binary at build/src/aid
build/src/aid /etc/aid-daemon/config.json
```

The daemon takes exactly one argument: the path to its config file. The full schema
is in [Configuration](07-configuration.md); at a minimum it needs the `Plugins`,
`TicketSystem`, `AddressSystem`, `TicketRouting`, `Logger`, and `lanInterface`
sections, plus a `listenPort` (default `80` — use a high port like `8080` on a dev
box).

On a clean start you'll watch the plugins load, the three ABI checks pass, the WAL
replay, and the listeners come up on `127.0.0.1` and your `lanInterface`.

## 10.3 Confirm it's alive

```sh
curl -s http://127.0.0.1:8080/health | jq
```

```jsonc
{ "status": "ok", "pluginsLoaded": true, "ticketSystem": "ok",
  "addressSystem": "ok", "uptimeS": 3, "queuedEvents": 0, "failedEvents": 0 }
```

`pluginsLoaded: true` together with `ticketSystem`/`addressSystem` at `"ok"` means
both backends are reachable. If either one isn't `"ok"`, sort out connectivity or
credentials before you send any calls (see
[Troubleshooting](12-troubleshooting-and-glossary.md)).

## 10.4 Send your first call

The simplest test there is — a single incoming call — with `curl`:

```sh
curl -i -X POST http://127.0.0.1:8080/call \
  -H 'Content-Type: application/json' \
  -d '{"event":"Incoming Call","remote":"+4915112345678","callid":"demo-1","dialed":"+493022220"}'
# → HTTP/1.1 202 Accepted
```

`202` means *durably accepted and queued* — the ticket gets created a moment later,
asynchronously. Check your ticket system and a `New` ticket should turn up, routed
per [§9.1](09-how-calls-become-tickets.md): in the caller's contact project if
`+4915112345678` is a known contact, otherwise in your `unknownFallback`.

## 10.5 A full call lifecycle

The repo ships a driver, `scripts/calltrigger.sh`, that posts the real wire shapes
so you don't have to hand-write JSON. It drives the inbound lifecycle, and all the
events you select share one `callid`.

Run the whole Incoming → Accepted → Hangup sequence for one call, assigned to
operator `alice`:

```sh
# -i incoming, -a accepted, -h hangup ; -u sets the accepted-call user
scripts/calltrigger.sh -iah -P 8080 -u alice  call-42  +4915112345678  +493022220
```

or step by step, watching the ticket change between each:

```sh
scripts/calltrigger.sh -i -P 8080            call-42  +4915112345678  +493022220
scripts/calltrigger.sh -a -P 8080 -u alice   call-42  +4915112345678  +493022220
scripts/calltrigger.sh -h -P 8080            call-42  +4915112345678
```

What you should see in the ticket after each step — this is the trace from
[§9.5](09-how-calls-become-tickets.md):

1. **after `-i`** — a `New` ticket, unassigned, `callIds=[call-42]`.
2. **after `-a`** — status `InProgress`, `assignee=alice`, a call-start line in the
   call log, `alice` among the call handlers. (This requires `alice` to exist as
   both a dashboard user and a ticket-system user —
   [§9.4](09-how-calls-become-tickets.md) — otherwise the accept is silently dropped
   and the ticket stays `New`.)
3. **after `-h`** — the call-log line gains `Call End: …`, `callIds` empties, and the
   ticket *stays* `InProgress` (hangup doesn't close it).

> For an **Outgoing Call** — which the driver doesn't cover — POST the shape
> described in [Integrating the Call API](02-integrating-call-api.md) directly. It has
> `user` and no `dialed`, and creates an already-`InProgress`, already-assigned ticket
> in a single step.

## 10.6 Did it work? Where to look

- **The ticket system** — the ticket itself is the source of truth, since the daemon
  keeps no DB of call state.
- **`GET /health`** — `queuedEvents` should settle back to `0`; a rising
  `failedEvents` means events are erroring and being held for replay.
- **`backend.log`** (path from your `Logger` config) — decode failures, WAL issues,
  and use-case errors land here, each tagged with a correlation id. Bear in mind it's
  quiet on success by design.
- **The dashboard** — if you set `Ui.documentRoot`, log in and watch tickets appear
  and update live over the WebSocket.

If a `202` didn't produce the ticket you expected, the
[Troubleshooting FAQ](12-troubleshooting-and-glossary.md) walks through the usual
suspects — a silently-dropped unknown user, the wrong routing project, an
accept-before-incoming.

## 10.7 Wiring a real phone system

Your PBX, or a small bridge script in front of it, POSTs the five shapes as calls
happen. A typical bridge maps your PBX's events (for example Asterisk AMI → `/call`)
to the wire format; use the field reference below for the mapping. The essentials:

- Use one stable, unique `callid` per call across all of its events (Incoming,
  Accepted, Hangup) so they attach to the same ticket.
- Send events in the order the PBX emits them — the daemon processes a given `callid`
  strictly in order and won't buffer to wait for an earlier event.
- Point the bridge at `POST /call` on the daemon's port (and mind the proxy note in
  [§2.1](02-integrating-call-api.md)).

---

Next: [Integrating the Call API →](02-integrating-call-api.md)
