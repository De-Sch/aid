# 12. Troubleshooting & glossary

[← Building, testing & scripts](11-building-testing-and-scripts.md) · [Back to index](README.md)

## 12.1 FAQ

### I got `202` but no ticket appeared

A `202` only means *durably accepted and queued* — the ticket gets created
asynchronously afterward, and that step can fail or quietly no-op. Check these, in
order:

1. **`GET /health`** — is `ticketSystem: "ok"`? If it isn't, the backend is
   unreachable or the credentials are wrong, and the create is failing upstream.
   Keep an eye on `failedEvents`: a climbing count means events are erroring and
   being held in the WAL.
2. **`backend.log`** — a failed create or save shows up here with a correlation id
   and an error code. (The log is silent on success by design, so "nothing in the
   log" after a successful call is exactly what you'd expect.)
3. **Was it an update event with no ticket to update?** `Accepted Call` and
   `Transfer Call` are silent no-ops when no ticket exists yet for that `callid`
   ([§9.2](09-how-calls-become-tickets.md)). Only `Incoming`/`Outgoing` create one.
4. **Was the operator unknown?** If the event carried a `user`/`newuser` that isn't
   present in *both* the auth DB and the ticket system, the event is silently
   dropped ([§9.4](09-how-calls-become-tickets.md)) — no ticket change, and no error
   back to you.

### The ticket landed in the wrong project

Routing is contact-driven, not payload-driven
([§9.1](09-how-calls-become-tickets.md)):

- The caller has to be a contact in the address book, and that contact has to have
  the target project assigned (`projectIds`). A contact with no assigned project
  counts as *Unknown* and routes to `TicketRouting.unknownFallback`.
- If the number is withheld or unparseable, `canonicalize` returns empty, which
  makes it *Incognito* — always `unknownFallback`, with subject `incognitoSubject`.
- So "wrong project" almost always means the contact wasn't found (a number-format
  mismatch — check the E.164 canonicalization) or the contact has no projects, or
  different ones than you thought.

### The accept didn't assign the operator

- The `user` has to resolve in *both* stores — the auth DB and the ticket system —
  spelled identically ([§9.4](09-how-calls-become-tickets.md)). An unresolved user
  drops the whole accept silently.
- Assignment is first-handler-wins: if the ticket already had an assignee, a later
  accept or transfer records the operator in `callHandlers` but doesn't overwrite
  the assignee.

### The call hung up but the ticket is still open

That's by design — `Hangup` never closes the ticket
([§9.2](09-how-calls-become-tickets.md)). It records the end time and completes the
call-log line, but the ticket stays `InProgress`. Operators close tickets by hand
from the dashboard (`/ui/close`), which runs the two-step `New → InProgress →
Closed` walk.

### Events for one call seem to apply out of order

The daemon processes a given `callid` strictly in order, but it doesn't reorder or
buffer. Emit an `Accepted` ahead of its `Incoming` under the same `callid` and the
accept no-ops (there's no ticket yet). Send events in the order the PBX produces
them, and reuse one stable `callid` across a call's whole lifetime.

### A transfer did nothing

A `Transfer Call`'s `callid` may be a different Uniqueid from the original leg, so
the ticket is found by *substring* match over its `callIds`. If that id isn't part
of an open ticket, the transfer silently no-ops
([§9.2](09-how-calls-become-tickets.md)).

### `/call` returns `503`

Backpressure. Either the per-`callid` queue is full (32), the process-wide mailbox
cap (10 000) is reached, or the daemon is draining for shutdown
([§8.2](08-operational-model.md)). Retry shortly.

### `/call` returns `400` / `500`

- `400` — the body didn't decode: bad JSON, a missing or non-string required field,
  or an unrecognized `event` string. Recheck the exact shape and event names in
  [§2.3](02-integrating-call-api.md) — remember `"Hangup"` with no `" Call"`, and
  `newuser` in lowercase.
- `500` — the WAL append or `fdatasync` failed (disk full, say). The event was *not*
  accepted; resend once you've fixed the disk.

### The daemon won't start

- **Config rejected** — mode wider than `0640`, wrong owner, or a malformed section
  ([§7.4](07-configuration.md)). The single stderr line tells you which.
- **Plugin refused** — an ABI check failed: a wrong or absent layout tag or contract
  tag, or the `.so` is world/group-writable or owned by the wrong user
  ([§5.3](05-writing-a-plugin.md), [§5.6](05-writing-a-plugin.md)). Rebuild and
  redeploy the plugin against the same source tree as the daemon.
- **Preflight** — a log, WAL, or DB directory isn't writable, or a plugin `.so` path
  is missing.

## 12.2 Reading the signals

| Signal | Where | Healthy state |
|---|---|---|
| Liveness + reachability | `GET /health` | `status:"ok"`, both backends `"ok"`, `queuedEvents` returns to `0` |
| Errors with correlation id | `backend.log` | silent on success; errors carry a code + cid |
| Durability backlog | `inbox.log` / `webhook.log` | compacts to **0 bytes** when all events are acked — empty is healthy |
| Failed events kept for replay | `/health` `failedEvents` | `0`; a rising count means upstream errors |
| Live dashboard updates | browser WebSocket `/ui/stream` | `ticket_upsert` / `ticket_remove` frames as tickets change |

## 12.3 Glossary

Every term the rest of the guide leans on, in one place — from the phone side of the
wire through to the dashboard. Roughly alphabetical, so it reads as a reference you
can dip into rather than front to back.

| Term | Meaning |
|---|---|
| **`202` / `400` / `500` / `503`** | The four ways `/call` (and the webhook) can answer. `202` = durably accepted and queued; `400` = the body didn't decode; `500` = the WAL write couldn't be synced, so the event was lost; `503` = backpressure, retry shortly. |
| **ABI** | Application binary interface — the binary-level contract between the daemon and a plugin `.so`: struct layouts, the factory signature, the guard symbols. The two are built separately, so they have to agree on it exactly. |
| **adapter / plugin** | A concrete implementation of a port, shipped as a `dlopen`'d `.so` (OpenProject for tickets, DaviCal for contacts). Loaded once at startup and held until the process exits. |
| **AMI (Asterisk Manager Interface)** | The Asterisk event stream the reference bridge `calls.py` listens to, turning raw phone events into `POST /call` requests. |
| **assignee** | The one operator a ticket is assigned to. Set the first time someone handles the call and never overwritten afterwards (first-handler-wins); later handlers are tracked separately in `callHandlers`. |
| **backpressure** | The daemon answering `503` when it's momentarily overloaded — a queue is full, the mailbox cap is hit, or it's shutting down. It means "not right now, retry shortly," not "failed." |
| **call-log line (`callLength`)** | A human-readable breadcrumb the daemon writes per call into a ticket's `callLength` field (`alice: Call start: … Call End: …`). Kept apart from operator comments, which live in `description`. It is *not* a duration. |
| **callid** | The unique id of a phone call (Asterisk's `Uniqueid`). It doubles as the mailbox key — every event sharing a callid is processed in order and attaches to one ticket. A transfer's callid may differ from the original leg, which is why tickets are matched by substring. |
| **canonicalize** | Rewriting a raw incoming number into strict E.164 (`+` country code, digits only) before it's looked up in the address book. A withheld or unparseable number canonicalizes to empty, which makes the call *incognito*. |
| **CardDAV** | The open standard for reading and writing address books over HTTP. AID's contacts live in CardDAV collections; DaviCal is the server that hosts them. |
| **contract tag / layout tag** | Two of the ABI guards a plugin `.so` must export. The layout tag fingerprints the memory layout of every boundary struct; the contract tag is a hand-bumped number for behavioural changes. A mismatch on either refuses startup. |
| **correlation id (cid)** | A short id pinned to one request and echoed on every log line it touches, so you can follow a single call's path through `backend.log`. |
| **DaviCal** | The CardDAV server that holds the two address books, reached through the shipped `aid_davical_plugin.so`. Swappable for any other `AddressBook` backend. |
| **domain loop** | The single dedicated event loop that runs all mailbox workers and both plugins' HTTP clients, kept separate from the socket-accept threads so upstream latency never blocks accepting new events. |
| **Drogon** | The C++ web framework the daemon is built on (HTTP, WebSocket, async client, coroutines). It's confined to the daemon shell — ports, domain logic, and plugins never see it. |
| **E.164** | The international phone-number format: a leading `+`, country code, then digits, no spaces or brackets (`+4915112345678`). Every `TEL` in the address book must be stored this way or it won't match. |
| **echo suppression** | The webhook path ignoring a change the daemon itself just made, so its own writes don't come back around as fake "external edits." Matched on the exact `(ticketId, lockVersion)` pair, so a real human edit in the same window still gets through. |
| **first-handler-wins** | The rule that a ticket's assignee is set once — by whoever handles the call first — and is never overwritten by a later accept or transfer. |
| **handler / callHandler** | An operator who accepted, made, or received a transfer on a ticket, recorded in a CSV field. Drives dashboard visibility independently of the single assignee slot. |
| **health probe (`/health`)** | The unauthenticated `GET /health` endpoint that reports liveness, whether the plugins loaded, whether both backends are reachable, and the queue counts. |
| **hexagonal (ports & adapters)** | The architecture AID follows: business logic depends only on abstract ports, and swappable adapters do the actual I/O. It's what makes a backend a plugin swap rather than a rewrite. |
| **incognito subject / unknownFallback** | The `TicketRouting` config for the incognito and unknown paths: the subject line to use for withheld calls, and the project that unrouted calls land in. |
| **known / unknown / incognito** | The three routing outcomes: *known* = the caller is a contact with assigned projects; *unknown* = the number canonicalizes but matches no contact/project, so it goes to the fallback project; *incognito* = a withheld or unparseable number, which also goes to the fallback project but is never deduplicated. |
| **live delta (`ticket_upsert` / `ticket_remove`)** | The messages the daemon pushes over the dashboard WebSocket when a ticket changes, so boards update without a refresh. `ticket_upsert` adds or updates a row; `ticket_remove` drops one. |
| **lockVersion** | A ticket's optimistic-lock version. A stale write gets `409`, and the adapter retries. Also carried in live-delta frames so a browser can drop a frame that lost a race with a newer one. |
| **mailbox** | A per-key ordered queue. Events for one `callid` (or one ticket id, for webhooks) run on a single worker in order; different keys run in parallel. Bounded at 32, with `503` on overflow. |
| **membership / reconciler** | Which operators belong to which OpenProject project, and the background timer (`membershipPollIntervalSec`) that re-polls it so the dashboard reflects members being added or removed. |
| **OpenProject** | The ticket tracker AID drives out of the box, through the shipped `aid_openproject_plugin.so`. Swappable for any other `TicketStore` backend. |
| **optimistic locking** | The scheme where each ticket carries a `lockVersion`, and a write with a stale version is rejected (`409`) instead of silently clobbering someone else's change. The adapter absorbs the conflict and retries. |
| **PBX** | Your phone system (private branch exchange). It — or a small bridge like `calls.py` in front of it — is what POSTs call events to `/call`. |
| **port** | An abstract interface the core depends on: `TicketStore` (tickets), `AddressBook` (contacts), `UiNotifier` (dashboard pushes). The extension seam — implement one and you've bound a new backend. |
| **preflight** | The startup checks the daemon runs before opening any listener — log/WAL/DB directories writable, plugin `.so` files present and correctly owned. A failure aborts the start. |
| **project id / `X-CUSTOM1`** | The OpenProject project number(s) a contact's calls route to, stored in the contact's `X-CUSTOM1` vCard field as a comma-separated list. This is the routing source of truth. |
| **recipients** | The set of users who should see a ticket — project members ∪ call handlers, deduplicated. Decides who gets a live WebSocket delta. |
| **recovery key (master key)** | A master password an administrator sets at install time. Entered in the password box in place of a real password, it lets anyone set a new password for any dashboard account. |
| **reducer** | A pure `Ticket → Ticket` function passed to `TicketStore.save`. The adapter re-applies it against fresh server state on each optimistic-lock retry, so it has to express changes as a delta, never as a captured snapshot. |
| **routing** | Deciding which project a call's ticket belongs to. Driven entirely by the caller's address-book contact, not by anything in the `/call` payload. |
| **session / cookie** | A signed-in dashboard user's login state, held in a cookie (`aid_session` by default) and backed by the local `auth.db`. Configured under `Auth`. |
| **two-step close** | Closing a `New` ticket walks it `New → InProgress → Closed`, because OpenProject forbids a direct `New → Closed` jump. |
| **use case** | One orchestrator class per `/call` or `/ui` action (`HandleIncomingCall`, and so on), sitting between the controllers and the ports. It's where a call's actual behaviour lives. |
| **vCard** | The standard contact-record format (RFC 6350) each address-book entry is stored as. AID reads four fields from it: `FN`, `ORG`, `TEL`, and `X-CUSTOM1`. |
| **WAL (write-ahead log)** | The append-only durability log. Events are `fdatasync`'d to it before `/call` returns `202`, and replayed on the next startup. It compacts back to empty once everything is acked. |
| **webhook (`/hook/ticket`)** | The optional inbound endpoint OpenProject POSTs to when a ticket is edited directly in its own UI, so the dashboard can reflect that change live. Protected by a shared secret. |

---

[← Back to index](README.md)
