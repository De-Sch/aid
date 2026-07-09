/**
 * AID2.0 dashboard wire types.
 *
 * These JSDoc `@typedef`s mirror the locked backend contract EXACTLY. They carry
 * no runtime code — they exist so `svelte-check` can validate
 * every value that crosses the wire (no `any`), and so editors autocomplete it.
 *
 * Contract invariants:
 *   - Every id (ticketId, callId, statusId, user handle, phone number) is a STRING,
 *     even when it looks numeric ("42", not 42).
 *   - Optional fields are EITHER their value OR `null` — never omitted/undefined.
 *   - `message` is `null` (not omitted) when there is no message, in BOTH the REST
 *     response and the WebSocket frame.
 *
 * Reference these from other modules with a JSDoc import, e.g.
 *   import('$lib/api/types.js').DashboardView
 */

/**
 * Ticket-system status, mapped to a fixed enum string.
 * "Unknown" is a backend fallback that should never appear in practice.
 * @typedef {"New" | "InProgress" | "Closed"} TicketStatus
 */

/**
 * Kind of address-book match for a resolved contact.
 * @typedef {"Person" | "Company"} AddressKind
 */

/**
 * The action a result belongs to.
 * @typedef {"COMMENT_SAVE" | "TICKET_CLOSE"} ActionOp
 */

/**
 * One row in the dashboard ticket list (`tickets[]`).
 * @typedef {object} DashboardEntry
 * @property {string} id                       Ticket id (ticket-system work package). Use for /ui/comment/{id} and /ui/close/{id}.
 * @property {string} subject                  Display title of the ticket.
 * @property {TicketStatus} status             Status enum string.
 * @property {string} statusId                 Raw ticket-system status id (string).
 * @property {string[]} callIds                All call ids on this ticket; may be empty [].
 * @property {string} callerNumber             Caller's phone number.
 * @property {string | null} calledNumber      Called number, or null if unknown.
 * @property {string | null} assignee          Assigned user handle, or null.
 * @property {string | null} callStart         Call start as the daemon's local wall-clock "YYYY-MM-DD HH:MM:SS", or null. Display verbatim — no timezone conversion.
 * @property {string | null} callEnd           Call end as the daemon's local wall-clock "YYYY-MM-DD HH:MM:SS", or null. Display verbatim — no timezone conversion.
 * @property {string} href                     Deep link into the ticket-system ticket.
 * @property {string} projectName              Project the ticket lives in (display label).
 * @property {string | null} activeCallForViewer  Call id if a call is live for THIS viewer on this ticket, else null.
 * @property {string[]} otherActiveUsers       Other users (not the viewer) with a live call on this ticket; may be empty []. Rendered uncolored.
 * @property {string} description              Human-typed comments only (auto call-log lines are stored in the backend `callLength` field, not sent here); "" when empty.
 * @property {string} updatedAt                Last-modified instant as ISO-8601 UTC ("YYYY-MM-DDTHH:MM:SSZ"). Sort key only (never displayed): the live-delta merge re-orders rows by status rank → updatedAt desc → id, mirroring the server.
 * @property {number} [lockVersion]            Ticket-system optimistic-locking version. Absent on the REST snapshot (rides at the WS frame top level); the stream client stamps it onto upserted entries so the merge can drop stale frames.
 */

/**
 * The viewer's current live call (`active`), or null when none.
 * The backend takes the first ticket whose `activeCallForViewer` is set and summarizes it.
 * @typedef {object} ActiveCall
 * @property {string} ticketId
 * @property {string} callId
 * @property {string} projectName              Extracted from the ticket href (/projects/<X>/).
 * @property {string} callerNumber
 */

/**
 * Structured contact auto-resolved from the address book for the active caller
 * (`addressCallInformation`), or null when there is no active call or no match.
 * @typedef {object} Contact
 * @property {string} name                     Display name (FN from the vCard).
 * @property {string} companyName              Company name (ORG); may be "" if none.
 * @property {AddressKind} kind                "Person" or "Company".
 * @property {string[]} phoneNumbers           All TEL entries (strings); may hold several.
 * @property {string[]} projectIds             Project ids (X-CUSTOM1); may be empty [].
 */

/**
 * The whole `GET /ui/dashboard` payload.
 * @typedef {object} DashboardView
 * @property {DashboardEntry[]} tickets
 * @property {ActiveCall | null} active
 * @property {Contact | null} addressCallInformation
 */

/**
 * Result of a comment/close action (REST body, and mirrored in the WS frame).
 * @typedef {object} ActionResult
 * @property {boolean} ok                       true on success; false is a business failure (not an HTTP error).
 * @property {ActionOp} op
 * @property {string} ticketId
 * @property {string | null} message            Extra text, or null when there is none.
 */

/**
 * `GET /ui/whoami` success body.
 * @typedef {object} WhoAmI
 * @property {string} username
 */

/**
 * Plain `{ ok: true }` acknowledgement (login/logout success).
 * @typedef {object} OkAck
 * @property {true} ok
 */

/**
 * Response from `POST /ui/login`. Either `{ ok: true }` on a normal login,
 * or `{ resetRequired: true }` when the supplied password was the recovery
 * key — in which case an `aid_reset` grant cookie was set instead of a
 * session and the caller should navigate to `/reset`.
 * @typedef {object} LoginAck
 * @property {true} [ok]
 * @property {true} [resetRequired]
 */

/** @typedef {"ok" | "degraded" | "starting"} HealthStatus */
/** @typedef {"reachable" | "unreachable"} Reachability */

/**
 * `GET /health` payload (no auth). Keys are camelCase; numeric fields are real numbers.
 * @typedef {object} Health
 * @property {HealthStatus} status
 * @property {boolean} pluginsLoaded
 * @property {Reachability} ticketSystem
 * @property {Reachability} addressSystem
 * @property {number} uptimeS                   Seconds since start.
 * @property {number} queuedEvents              Events currently waiting in mailboxes.
 * @property {number} failedEvents              Events that failed.
 */

/* ── WebSocket /ui/stream frames (server-push only) ───────────────────────────
 * The daemon pushes small signals (no client→server frames are read):
 *   - invalidate     — "reload that view" (coarse fallback; full GET refetch).
 *   - action_result  — "your action finished".
 *   - ticket_upsert  — a single dashboard row changed for THIS viewer; merge it.
 *   - ticket_remove  — a row left this viewer's board (status no longer
 *                      New/InProgress, or no longer their concern); drop it.
 * The upsert/remove deltas let a client update one row in place instead of
 * refetching the whole board; the full GET /ui/dashboard stays the snapshot of
 * record on login / refresh / reconnect.
 */

/**
 * "Reload" signal. React by re-running the matching request (usually GET /ui/dashboard).
 * @typedef {object} InvalidateFrame
 * @property {"invalidate"} type
 * @property {string} scope                     What to reload, e.g. "dashboard".
 */

/**
 * Result of one user's action, pushed to that user. `message` behaves like REST: null when none.
 * @typedef {object} ActionResultFrame
 * @property {"action_result"} type
 * @property {ActionOp} op
 * @property {string} ticketId
 * @property {boolean} ok
 * @property {string | null} message
 */

/**
 * A single dashboard row created/changed for the receiving viewer. The embedded
 * `entry` is byte-identical to a REST `DashboardEntry` (so the UI has one
 * parser); `lockVersion` rides at the top level (not inside `entry`) so the
 * merge can drop a frame that lost a race with a newer one for the same ticket.
 * @typedef {object} TicketUpsertFrame
 * @property {"ticket_upsert"} type
 * @property {DashboardEntry} entry
 * @property {number} lockVersion               Post-save ticket-system version of `entry`.
 */

/**
 * A dashboard row that left the receiving viewer's board. Drop it by id,
 * version-guarded against a newer upsert already applied for the same ticket.
 * @typedef {object} TicketRemoveFrame
 * @property {"ticket_remove"} type
 * @property {string} ticketId
 * @property {number} lockVersion               Post-save version at the time of removal.
 */

/**
 * Any frame the server may push on /ui/stream.
 * @typedef {InvalidateFrame | ActionResultFrame | TicketUpsertFrame | TicketRemoveFrame} StreamFrame
 */

// This module is JSDoc-only; the export makes it an ES module so the typedefs
// above are importable via `import('$lib/api/types.js').<Name>`.
export {};
