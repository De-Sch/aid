/**
 * `/ui/stream` WebSocket client — server-push only.
 *
 * The daemon uses an **invalidate-and-refetch** protocol on this socket: it
 * never sends ticket data,
 * only tiny signals — `{type:"invalidate",scope}` ("reload that view") and
 * `{type:"action_result",…}` ("your action finished") — and it pings every 30 s.
 * It buffers nothing across reconnects, so the single rule is: **on every
 * (re)connect, refetch** via the REST `client`. Callers hook `onConnect` for that.
 *
 * This client is strictly inbound: it never calls `ws.send()`. Frames the server
 * does not define (the keep-alive ping, anything malformed) are ignored.
 *
 * It owns only the socket + reconnect-with-backoff. State lives in the stores.
 */
import type { ActionResultFrame, DashboardEntry, StreamFrame } from './types.js';

/** Callbacks the owner (dashboard store) wires to refetch / surface events. */
export interface StreamHandlers {
	/** Socket opened — first connect AND every reconnect. Refetch here. */
	onConnect?: () => void;
	/** Socket closed/errored; a reconnect is being scheduled. */
	onDisconnect?: () => void;
	/** `{type:"invalidate"}` received. `scope` names what to reload (e.g. "dashboard"). */
	onInvalidate?: (scope: string) => void;
	/** `{type:"action_result"}` received — the result of one of this user's actions. */
	onActionResult?: (frame: ActionResultFrame) => void;
	/**
	 * `{type:"ticket_upsert"}` received — merge this single row. The frame's
	 * top-level `lockVersion` is stamped onto the entry so the store can guard
	 * against stale frames (the embedded `entry` itself carries none).
	 */
	onTicketUpsert?: (entry: DashboardEntry) => void;
	/** `{type:"ticket_remove"}` received — drop this row (version-guarded). */
	onTicketRemove?: (ticketId: string, lockVersion: number) => void;
}

/** Handle returned by {@link createStream}. */
export interface StreamClient {
	/** Open the socket (idempotent) and keep it open across drops. */
	start(): void;
	/** Close for good — no further reconnects. */
	stop(): void;
}

/** Backoff: 1s, 2s, 4s, 8s, 16s, then capped at 30s. Reset on a clean open. */
const BACKOFF_BASE_MS = 1000;
const BACKOFF_CAP_MS = 30_000;

/** Same-origin ws/wss URL for the stream (cookie rides the upgrade). */
function streamUrl(): string {
	const proto = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
	return `${proto}//${window.location.host}/ui/stream`;
}

/**
 * Create a reconnecting `/ui/stream` client. Inbound-only.
 */
export function createStream(handlers: StreamHandlers = {}): StreamClient {
	let ws: WebSocket | null = null;
	let timer: ReturnType<typeof setTimeout> | null = null;
	let attempt = 0;
	let stopped = false;

	function clearTimer(): void {
		if (timer !== null) {
			clearTimeout(timer);
			timer = null;
		}
	}

	function scheduleReconnect(): void {
		if (stopped) return;
		const delay = Math.min(BACKOFF_CAP_MS, BACKOFF_BASE_MS * 2 ** attempt);
		attempt += 1;
		clearTimer();
		timer = setTimeout(connect, delay);
	}

	function handleMessage(ev: MessageEvent): void {
		// App frames are JSON text. Anything else (keep-alive, garbage) is ignored;
		// protocol-level pings are answered by the browser and never reach us.
		if (typeof ev.data !== 'string') return;
		let frame: StreamFrame;
		try {
			frame = JSON.parse(ev.data);
		} catch {
			return;
		}
		if (!frame || typeof frame !== 'object') return;
		if (frame.type === 'invalidate') {
			handlers.onInvalidate?.(frame.scope);
		} else if (frame.type === 'action_result') {
			handlers.onActionResult?.(frame);
		} else if (frame.type === 'ticket_upsert') {
			// lockVersion rides at the frame top level (not inside entry); stamp it
			// onto the entry so the store's merge can drop a frame that lost a race.
			handlers.onTicketUpsert?.({ ...frame.entry, lockVersion: frame.lockVersion });
		} else if (frame.type === 'ticket_remove') {
			handlers.onTicketRemove?.(frame.ticketId, frame.lockVersion);
		}
		// Unknown type → ignore.
	}

	function connect(): void {
		if (stopped || typeof window === 'undefined') return;
		clearTimer();

		const socket = new WebSocket(streamUrl());
		ws = socket;

		socket.onopen = () => {
			if (stopped) {
				socket.close();
				return;
			}
			attempt = 0; // clean connection — reset backoff
			handlers.onConnect?.();
		};

		socket.onmessage = handleMessage;

		// close fires after error too, so schedule the reconnect there only.
		socket.onclose = () => {
			if (ws === socket) ws = null;
			handlers.onDisconnect?.();
			scheduleReconnect();
		};

		socket.onerror = () => {
			// Let onclose drive reconnection; closing here guarantees it fires.
			socket.close();
		};
	}

	return {
		start(): void {
			if (typeof window === 'undefined') return; // SSR guard (ssr=false, but be safe)
			stopped = false;
			if (ws || timer) return; // already running / scheduled
			attempt = 0;
			connect();
		},
		stop(): void {
			stopped = true;
			clearTimer();
			if (ws) {
				// Drop handlers so the imminent close doesn't trigger a reconnect.
				ws.onopen = ws.onmessage = ws.onclose = ws.onerror = null;
				ws.close();
				ws = null;
			}
		}
	};
}
