/**
 * Dashboard state — the single source of truth for the operator view.
 *
 * The view is seeded by one REST call (`client.getDashboard()`) and then kept
 * live by per-row deltas over the `/ui/stream` WebSocket:
 *   - every (re)connect              -> refetch the full snapshot (recovery; the
 *                                       server buffers nothing across drops)
 *   - ticket_upsert                  -> MERGE one row into `tickets` in place
 *   - ticket_remove                  -> drop one row from `tickets`
 *   - invalidate {scope:"dashboard"} -> refetch (coarse fallback)
 *   - action_result                  -> push a toast
 *
 * The merge keeps `tickets` in the server's order (status rank → updatedAt desc
 * → id) and is version-guarded, so a single changed ticket updates only its own
 * row and unrelated rows persist untouched — no whole-board refetch per change.
 * The full GET stays the snapshot of record (login / refresh / reconnect).
 *
 * A 401 on a refetch means the session lapsed: we navigate to /login and let the
 * existing layout gate (+layout.ts) do the real auth check on arrival.
 *
 * Runes module (`.svelte.ts`): the `$state` below is shared app-wide.
 */
import { goto } from '$app/navigation';
import { client, AuthError } from '$lib/api/client.js';
import { createStream, type StreamClient } from '$lib/api/stream.js';
import { upsertEntry, removeEntry, deriveActive } from './dashboard-merge.js';
import { toasts } from './toasts.svelte';
import type { ActionResultFrame, ActiveCall, Contact, DashboardEntry } from '$lib/api/types.js';

let tickets = $state<DashboardEntry[]>([]);
let active = $state<ActiveCall | null>(null);
let addressCallInformation = $state<Contact | null>(null);
let loading = $state(false);
let error = $state<string | null>(null);
let initialized = $state(false);
let connected = $state(false);

let stream: StreamClient | null = null;
/** Coalesces concurrent refreshes (start() + first onConnect) into one fetch. */
let inFlight: Promise<void> | null = null;

/**
 * Re-derive `active` from the just-merged `tickets` so the spotlight panel
 * (ActiveCallSpotlight) tracks live deltas instead of only the last full
 * fetch. When the resolved caller actually changes (a call started/ended/
 * moved to a different number), the CardDAV contact card can't be derived
 * client-side — drop the stale one and let one background refresh refill it.
 */
function syncActiveFromTickets(): void {
	const next = deriveActive(tickets);
	const callerChanged = (active?.callerNumber ?? null) !== (next?.callerNumber ?? null);
	active = next;
	if (callerChanged) {
		addressCallInformation = null;
		if (next) void dashboard.refresh();
	}
}

/** Map an action_result frame to a short, human toast line. */
function actionResultMessage(f: ActionResultFrame): string {
	if (f.message) return f.message;
	const ref = `#${f.ticketId}`;
	if (f.op === 'COMMENT_SAVE') {
		return f.ok ? `Comment saved on ${ref}` : `Could not save comment on ${ref}`;
	}
	if (f.op === 'TICKET_CLOSE') {
		return f.ok ? `Ticket ${ref} closed` : `Could not close ticket ${ref}`;
	}
	return f.ok ? `Action done on ${ref}` : `Action failed on ${ref}`;
}

async function doRefresh(): Promise<void> {
	loading = true;
	error = null;
	try {
		const d = await client.getDashboard();
		tickets = d.tickets;
		active = d.active;
		addressCallInformation = d.addressCallInformation;
		initialized = true;
	} catch (e) {
		if (e instanceof AuthError) {
			// Session lapsed — hand off to the auth gate.
			void goto('/login');
			return;
		}
		error = 'Could not load the dashboard.';
	} finally {
		loading = false;
	}
}

export const dashboard = {
	/** Ticket rows (DashboardEntry[]); empty until the first load. */
	get tickets(): DashboardEntry[] {
		return tickets;
	},
	/** The viewer's active call summary, or null. */
	get active(): ActiveCall | null {
		return active;
	},
	/** Resolved contact for the active caller, or null. */
	get addressCallInformation(): Contact | null {
		return addressCallInformation;
	},
	/** True while a dashboard fetch is in flight. */
	get loading(): boolean {
		return loading;
	},
	/** Non-auth load error message, or null. */
	get error(): string | null {
		return error;
	},
	/** True once the first successful load has populated the view. */
	get initialized(): boolean {
		return initialized;
	},
	/** True while the `/ui/stream` socket is open. */
	get connected(): boolean {
		return connected;
	},

	/**
	 * Refetch the dashboard. Concurrent calls share one in-flight request, so the
	 * start()+onConnect pair on first mount only hits the network once.
	 */
	refresh(): Promise<void> {
		if (inFlight) return inFlight;
		inFlight = doRefresh().finally(() => {
			inFlight = null;
		});
		return inFlight;
	},

	/**
	 * Begin live updates: open the stream and refetch on every (re)connect.
	 * Idempotent — calling twice is a no-op.
	 */
	start(): void {
		if (stream) return;
		stream = createStream({
			onConnect: () => {
				connected = true;
				void dashboard.refresh();
			},
			onDisconnect: () => {
				connected = false;
			},
			onInvalidate: (scope) => {
				if (scope === 'dashboard') void dashboard.refresh();
			},
			onActionResult: (f) => {
				toasts.push({ kind: f.ok ? 'success' : 'error', message: actionResultMessage(f) });
			},
			onTicketUpsert: (entry) => {
				// Merge the single changed row (version-guarded, re-sorted). Reassign
				// the $state array so the new reference triggers reactivity.
				tickets = upsertEntry(tickets, entry);
				syncActiveFromTickets();
			},
			onTicketRemove: (ticketId, lockVersion) => {
				tickets = removeEntry(tickets, ticketId, lockVersion);
				syncActiveFromTickets();
			}
		});
		stream.start();
	},

	/** Stop live updates and close the stream. */
	stop(): void {
		stream?.stop();
		stream = null;
		connected = false;
	}
};
