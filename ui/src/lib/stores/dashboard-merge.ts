/**
 * Pure dashboard-row merge logic for the live-delta protocol.
 *
 * The dashboard store keeps a `tickets` array seeded by the REST snapshot
 * (`GET /ui/dashboard`, already server-sorted). Over `/ui/stream` the daemon
 * then pushes single-row deltas — `ticket_upsert` / `ticket_remove` — for the
 * receiving viewer. These functions fold one delta into a tickets array and
 * return a NEW array kept in the SAME order a fresh snapshot would have:
 *
 *     status rank (New<InProgress<Closed)  →  updatedAt desc  →  id asc
 *
 * which mirrors the server's `OpDashboardBuilder` sort byte-for-byte (it sorts
 * on the raw Timestamp; updatedAt is serialized as fixed-width ISO-8601 UTC, so
 * our lexicographic compare matches its chronological one).
 *
 * Everything here is pure (no runes, no I/O) so it unit-tests in plain Node.
 */
import type { ActiveCall, DashboardEntry, TicketStatus } from '../api/types.js';

/** Display/sort rank for a status — matches OpDashboardBuilder's `statusRank`. */
function statusRank(s: TicketStatus): number {
	switch (s) {
		case 'New':
			return 0;
		case 'InProgress':
			return 1;
		case 'Closed':
			return 2;
		default:
			return 99;
	}
}

/**
 * Total order over rows, identical to the server's: status rank, then updatedAt
 * descending (newest first), then id ascending (lexicographic, as the backend
 * compares `id.v` strings) for a stable tiebreak.
 */
export function compareEntries(a: DashboardEntry, b: DashboardEntry): number {
	const ra = statusRank(a.status);
	const rb = statusRank(b.status);
	if (ra !== rb) return ra - rb;
	if (a.updatedAt !== b.updatedAt) return a.updatedAt < b.updatedAt ? 1 : -1; // desc
	if (a.id !== b.id) return a.id < b.id ? -1 : 1;
	return 0;
}

/**
 * Fold a `ticket_upsert` into `list`. Replaces the row with the same id when the
 * incoming version is newer, inserts it when absent, then re-sorts. Returns the
 * original array reference unchanged when the frame is stale (so the store skips
 * a needless reactive update).
 *
 * The version guard accepts the entry when the existing row has no lockVersion
 * (a REST-snapshot row never carries one) or when the incoming version is
 * strictly greater (a re-delivered equal version is a no-op).
 */
export function upsertEntry(list: DashboardEntry[], entry: DashboardEntry): DashboardEntry[] {
	const idx = list.findIndex((t) => t.id === entry.id);
	if (idx === -1) {
		return [...list, entry].sort(compareEntries);
	}
	const existing = list[idx];
	const fresh =
		existing.lockVersion == null ||
		entry.lockVersion == null ||
		entry.lockVersion > existing.lockVersion;
	if (!fresh) return list; // stale frame lost the race — drop it
	const next = list.slice();
	next[idx] = entry;
	return next.sort(compareEntries);
}

/**
 * Fold a `ticket_remove` into `list` — drop the row with `ticketId`. Returns the
 * original reference unchanged when the id is absent or when a newer upsert has
 * already been applied for it (a remove never undoes a strictly-newer version).
 */
export function removeEntry(
	list: DashboardEntry[],
	ticketId: string,
	lockVersion: number
): DashboardEntry[] {
	const idx = list.findIndex((t) => t.id === ticketId);
	if (idx === -1) return list;
	const existing = list[idx];
	const supersedes = existing.lockVersion == null || lockVersion >= existing.lockVersion;
	if (!supersedes) return list;
	return [...list.slice(0, idx), ...list.slice(idx + 1)];
}

/**
 * Recompute the viewer's active-call summary straight from `list`, mirroring
 * the server's own pick (`GetDashboard`: first row, in server sort order, with
 * `activeCallForViewer` set). `list` is already kept in that order by
 * `upsertEntry`/`removeEntry`, so this lets the store re-derive `active` after
 * every live delta instead of only on a full snapshot refetch.
 */
export function deriveActive(list: DashboardEntry[]): ActiveCall | null {
	for (const e of list) {
		if (e.activeCallForViewer != null) {
			return {
				ticketId: e.id,
				callId: e.activeCallForViewer,
				projectName: e.projectName,
				callerNumber: e.callerNumber
			};
		}
	}
	return null;
}
