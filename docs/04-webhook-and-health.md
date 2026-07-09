# 4. Webhook & Health

[← How calls become tickets](09-how-calls-become-tickets.md) · [Back to index](README.md)

Two smaller HTTP endpoints round out the integration surface: an optional inbound
webhook that reflects ticket-tracker edits back onto the live dashboard, and a
health probe.

## 4.1 `POST /hook/ticket` — reflect upstream edits

When an operator edits a ticket directly in OpenProject rather than through AID2.0,
the dashboard would otherwise drift out of date. OpenProject can be configured to
POST a work-package webhook to the daemon, and the daemon decodes it and pushes the
change to connected dashboards as a live delta.

The endpoint is optional. It's registered only when a `Webhook` section is present
in the config (see [Configuration](07-configuration.md)). Leave that section out and
the route simply doesn't exist — inbound webhooks are ignored.

### Authentication

Unlike `/call`, this endpoint *is* authenticated, using a shared secret. Present
the configured secret either as a header or as a query parameter:

```
POST /hook/ticket?secret=<shared-secret>
# or
POST /hook/ticket
X-AID-Webhook-Secret: <shared-secret>
```

The comparison is constant-time. A missing or wrong secret returns `401`, and the
secret is never logged.

### Body

The handler pulls a ticket id out of the JSON body. It accepts either an
OpenProject-style envelope with a `work_package` object, or a bare object with a
top-level `id`. The id can be an integer or a non-empty string:

```jsonc
// envelope form
{"action": "work_package:updated", "work_package": {"id": 1234, "...": "..."}}

// bare form
{"id": "1234"}
```

If no usable id can be extracted, the request comes back `400`.

### Responses

Same durability and backpressure model as `/call`:

| Code | Meaning |
|---|---|
| `202 Accepted` | webhook durably accepted and queued (on a separate `webhook.log` WAL, keyed by ticket id) |
| `401 Unauthorized` | missing/incorrect shared secret |
| `400 Bad Request` | body has no extractable ticket id |
| `500 Internal Server Error` | WAL append/fsync failed |
| `503 Service Unavailable` | backpressure (queue full / cap / draining) |

### What it does downstream

The queued webhook is decoded by the `TicketStore` plugin (`decodeWebhook`), which
suppresses the daemon's own echoes: if the incoming version matches a write the
daemon just made, nothing is emitted. A genuine external edit becomes
`ticket_upsert` / `ticket_remove` deltas, pushed to the affected operators'
dashboard WebSockets. That echo-suppression matches on the exact
`(ticketId, lockVersion)` pair, so a human edit that lands in the same window still
passes through.

## 4.2 `GET /health`

A dependency-free liveness/readiness probe. No authentication — same trust-the-LAN
model as `/call`. It always returns `200 OK` with a JSON body:

```jsonc
{
  "status": "ok",
  "pluginsLoaded": true,
  "ticketSystem": "ok",
  "addressSystem": "ok",
  "uptimeS": 4211,
  "queuedEvents": 0,
  "failedEvents": 0
}
```

| Key | Meaning |
|---|---|
| `status` | overall summary string |
| `pluginsLoaded` | both port plugins were `dlopen`'d and passed the ABI checks |
| `ticketSystem` | reachability of the ticket backend (from a cheap `ping`, cached from a cold-start probe at boot) |
| `addressSystem` | reachability of the address backend |
| `uptimeS` | seconds since start |
| `queuedEvents` | events currently queued across all mailboxes |
| `failedEvents` | events that errored and were left in the WAL for replay |

The keys are camelCase on the wire, matching the `/ui/*` payloads, even though the
internal snapshot struct uses snake_case. And because ticket/address reachability is
seeded by a cold-start ping issued at boot, `/health` is meaningful from the very
first second.

---

Next: [Writing a plugin →](05-writing-a-plugin.md)
