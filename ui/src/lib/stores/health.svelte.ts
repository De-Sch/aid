/**
 * Health state — feeds the status pill.
 *
 * Polls the unauthenticated `GET /health` on an interval.
 * No auth, so no redirect handling: a failed fetch just means the daemon is
 * unreachable, which is itself a status worth showing. The last good payload is
 * kept across a transient failure so the pill doesn't blank out.
 *
 * Runes module (`.svelte.ts`): the `$state` below is shared app-wide.
 */
import { client } from '$lib/api/client.js';
import type { Health } from '$lib/api/types.js';

const DEFAULT_INTERVAL_MS = 10_000;

let data = $state<Health | null>(null);
let reachable = $state(false);
let loading = $state(false);

let timer: ReturnType<typeof setInterval> | null = null;

async function poll(): Promise<void> {
	loading = true;
	try {
		data = await client.getHealth();
		reachable = true;
	} catch {
		// Daemon down / unreachable — keep the last payload, flag the pill.
		reachable = false;
	} finally {
		loading = false;
	}
}

export const health = {
	/** Last `GET /health` payload, or null before the first successful poll. */
	get data(): Health | null {
		return data;
	},
	/** False when the most recent poll could not reach the daemon. */
	get reachable(): boolean {
		return reachable;
	},
	/** True while a poll is in flight. */
	get loading(): boolean {
		return loading;
	},
	/** Convenience: daemon status string ("ok" | "degraded" | "starting"), or null. */
	get status(): Health['status'] | null {
		return data?.status ?? null;
	},
	/** Convenience: events currently queued in mailboxes, or null. */
	get queuedEvents(): number | null {
		return data?.queuedEvents ?? null;
	},
	/** Convenience: seconds since daemon start, or null. */
	get uptimeS(): number | null {
		return data?.uptimeS ?? null;
	},

	/** Begin polling (immediate first poll, then every `intervalMs`). Idempotent. */
	start(intervalMs: number = DEFAULT_INTERVAL_MS): void {
		if (timer !== null) return;
		void poll();
		timer = setInterval(() => void poll(), intervalMs);
	},

	/** Stop polling. */
	stop(): void {
		if (timer !== null) {
			clearInterval(timer);
			timer = null;
		}
	}
};
