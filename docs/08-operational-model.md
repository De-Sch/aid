# 8. Operational model

[← Configuration](07-configuration.md) · [Back to index](README.md)

This chapter covers the runtime guarantees an integrator leans on: how events are
made durable, how they're ordered and bounded, and how the daemon shuts down
cleanly. None of it takes code changes to use — it's simply the behaviour you're
integrating against.

## 8.1 Durability — the WAL

Every accepted `/call` — and every accepted webhook — is written to an append-only
write-ahead log and `fdatasync`'d to disk *before* the `202` goes back. That's the
durability contract: if the client saw `202`, the event survives a crash and gets
replayed on the next startup.

The log is a single `Wal` class (one file, `walPath`, default
`/var/lib/aid-daemon/inbox.log`; the webhook path is a sibling `webhook.log`).
Here's its lifecycle:

- **`append(body, cid)`** — writes one line, `fdatasync`s, and returns a sequence
  number. That sync is what gates the `202`. If it fails, the endpoint returns `500`
  and the event is *not* acknowledged.
- **`readAll()`** — at startup, before any listener opens, every un-acked record is
  replayed back through the mailbox. Since no listener is accepting yet and the
  domain loop isn't spinning, replay ordering holds.
- **`ack(seq)`** — once a worker successfully processes an event, its record is
  acked. Only the *contiguous* acked prefix is physically compacted from the front
  of the file, so an un-acked lower sequence belonging to a still-in-flight
  different call is never dropped.

One consequence is worth knowing: an event that *errors* during processing is left
un-acked in the WAL on purpose, so an operator can fix the cause and replay it.
That's exactly what `failedEvents` in `/health` counts.

> **Reading the WAL as an operator.** `inbox.log` and `webhook.log` compact down to
> zero bytes once everything is acked — an empty log is the *healthy* steady state,
> not a bug. A non-empty log means something is queued or has failed.

## 8.2 Ordering & backpressure — the mailbox

Events are dispatched through a per-key mailbox. For `/call` the key is the
`callid`; for webhooks it's the ticket id. The rules:

- **One worker per key, one event in flight.** All events that share a `callid` run
  strictly in order on a single worker coroutine. That's what guarantees Incoming →
  Accepted → Transfer → Hangup get applied in sequence.
- **Different keys run in parallel.** Two calls progress independently and
  concurrently on the shared domain loop.
- **Bounded queues.** Each key's queue holds at most 32 events; the 33rd is rejected
  with `503`.
- **Process-wide caps.** At most 10 000 live mailboxes exist at once (a distinct key
  past that cap gets `503`), and at most 500 simultaneous dashboard WebSocket
  connections.
- **Idle GC.** A mailbox with no activity for an hour is garbage-collected.

`Mailbox` (for calls) and `WebhookMailbox` (for webhooks) are thin typed facades
over one shared `MailboxEngine<Key, Payload>` template, so the concurrency and
lifetime machinery only gets written once.

## 8.3 The domain loop and the 409 retry

All mailbox workers and both plugin HTTP clients run on a single dedicated "domain
loop," kept separate from the listener threads that do
`accept → validate → WAL → 202`. Holding upstream latency off the accept path is
what lets `/call` stay fast under load.

OpenProject uses optimistic locking (`lockVersion`), so when two workers patch the
same ticket the loser gets a `409 Conflict`. The `TicketStore` adapter soaks this
up internally, invisibly to the use case: it re-fetches, re-applies the reducer, and
retries up to 5 times with 50/100/200/400/800 ms backoff, returning
`LockVersionExhausted` only when every attempt fails. This is exactly why `save`
takes a pure reducer (see [Value types §6.2](06-value-types.md)) — it may be
replayed against fresh server state on each attempt.

## 8.4 Live dashboard deltas

When a ticket changes, the use case calls `TicketDeltaEmitter`. It asks the
`TicketStore` who should see the ticket (`recipientsFor`), builds a per-viewer row
(`buildEntry`), and pushes it through the `UiNotifier` port to exactly those
operators' WebSocket connections:

- `pushTicketUpsert` → `{"type":"ticket_upsert","entry":{…},"lockVersion":N}`
- `pushTicketRemove` → `{"type":"ticket_remove","ticketId":"…","lockVersion":N}`

The `lockVersion` lets a browser drop a frame that lost a race with a newer one. The
concrete `UiNotifier` is the in-process `WsHubAdapter`, and it's where the
500-connection cap is enforced.

## 8.5 Startup & graceful shutdown

**Startup order** (`src/main.cpp`), abbreviated:

1. Tighten umask, load + stat-check config.
2. Initialize logging; run preflight (writability of log/WAL/db dirs, plugin `.so`
   existence and permissions).
3. Construct the domain loop; `dlopen` both plugins and run the three ABI checks.
4. Build in-process infrastructure (WAL, WebSocket hub), auth (SQLite), and the
   use cases.
5. **Replay the WAL before any listener opens.**
6. Optionally register the webhook ingest (if `Webhook` is configured).
7. Cold-start `/health` ping to the backends.
8. Register controllers and open the listeners (loopback + LAN).
9. Start timers (session prune, mailbox idle GC) and the optional membership
   reconciler.

**Shutdown** on `SIGTERM`/`SIGINT`:

1. Stop accepting new `/call` events.
2. Drain in-flight mailbox workers, up to a **10-second** budget.
3. Call `cancelPendingRequests()` on both plugins so any worker suspended inside an
   upstream request unwinds promptly, then a short settle drain.
4. Release the plugin instances (`destroy_*`) while the domain loop is still alive —
   their destructors may enqueue HTTP-client cleanup onto it — then exit.

Under `systemd`, set `KillMode=mixed` so the drain actually gets its 10 seconds
before `SIGKILL`.

---

Next: [Building, testing & scripts →](11-building-testing-and-scripts.md)
