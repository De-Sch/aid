import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { createStream } from './stream.js';

/**
 * Minimal fake WebSocket. Captures every instance and lets the test drive the
 * open/message/close events the real browser would fire. The stream client only
 * uses `new WebSocket(url)`, the four `on*` handlers, and `close()`.
 */
class FakeWebSocket {
	/** @type {FakeWebSocket[]} */
	static instances = [];

	/** @param {string} url */
	constructor(url) {
		this.url = url;
		/** @type {(() => void) | null} */
		this.onopen = null;
		/** @type {((ev: { data: unknown }) => void) | null} */
		this.onmessage = null;
		/** @type {(() => void) | null} */
		this.onclose = null;
		/** @type {(() => void) | null} */
		this.onerror = null;
		this.closed = false;
		FakeWebSocket.instances.push(this);
	}

	close() {
		this.closed = true;
	}

	// ── test drivers ──────────────────────────────────────────────────────────
	open() {
		this.onopen?.();
	}
	/** @param {unknown} data */
	message(data) {
		this.onmessage?.({ data });
	}
	/** Simulate the browser firing close (it always does, after error too). */
	drop() {
		this.onclose?.();
	}

	/** The most recently constructed socket. */
	static last() {
		return FakeWebSocket.instances[FakeWebSocket.instances.length - 1];
	}
}

// Node has no window/WebSocket; the cast lets us install fakes onto the global
// without fighting the DOM lib types (the client reads bare `window`/`WebSocket`).
const g = /** @type {any} */ (globalThis);

beforeEach(() => {
	FakeWebSocket.instances = [];
	vi.useFakeTimers();
	g.window = { location: { protocol: 'http:', host: 'localhost:5173' } };
	g.WebSocket = FakeWebSocket;
});

afterEach(() => {
	vi.useRealTimers();
	g.window = undefined;
	g.WebSocket = undefined;
});

describe('createStream — connection + frame dispatch', () => {
	it('opens a same-origin ws URL and fires onConnect on open', () => {
		const onConnect = vi.fn();
		const s = createStream({ onConnect });
		s.start();

		expect(FakeWebSocket.instances).toHaveLength(1);
		expect(FakeWebSocket.last().url).toBe('ws://localhost:5173/ui/stream');

		FakeWebSocket.last().open();
		expect(onConnect).toHaveBeenCalledOnce();
		s.stop();
	});

	it('uses wss when the page is https', () => {
		g.window.location.protocol = 'https:';
		const s = createStream({});
		s.start();
		expect(FakeWebSocket.last().url).toBe('wss://localhost:5173/ui/stream');
		s.stop();
	});

	it('dispatches an invalidate frame to onInvalidate with its scope', () => {
		const onInvalidate = vi.fn();
		const s = createStream({ onInvalidate });
		s.start();
		FakeWebSocket.last().open();

		FakeWebSocket.last().message(JSON.stringify({ type: 'invalidate', scope: 'dashboard' }));
		expect(onInvalidate).toHaveBeenCalledExactlyOnceWith('dashboard');
		s.stop();
	});

	it('dispatches an action_result frame to onActionResult', () => {
		const onActionResult = vi.fn();
		const s = createStream({ onActionResult });
		s.start();
		FakeWebSocket.last().open();

		const frame = { type: 'action_result', op: 'COMMENT_SAVE', ticketId: '42', ok: true, message: null };
		FakeWebSocket.last().message(JSON.stringify(frame));
		expect(onActionResult).toHaveBeenCalledExactlyOnceWith(frame);
		s.stop();
	});

	it('routes a ticket_upsert frame, stamping the top-level lockVersion onto the entry', () => {
		const onTicketUpsert = vi.fn();
		const s = createStream({ onTicketUpsert });
		s.start();
		FakeWebSocket.last().open();

		const entry = { id: '42', subject: 'Acme', status: 'New', updatedAt: '2026-06-09T10:00:00Z' };
		FakeWebSocket.last().message(JSON.stringify({ type: 'ticket_upsert', entry, lockVersion: 7 }));

		// The handler receives the entry with lockVersion merged in (it rides at
		// the frame top level, not inside entry).
		expect(onTicketUpsert).toHaveBeenCalledExactlyOnceWith({ ...entry, lockVersion: 7 });
		s.stop();
	});

	it('routes a ticket_remove frame to onTicketRemove with id + version', () => {
		const onTicketRemove = vi.fn();
		const s = createStream({ onTicketRemove });
		s.start();
		FakeWebSocket.last().open();

		FakeWebSocket.last().message(JSON.stringify({ type: 'ticket_remove', ticketId: '99', lockVersion: 12 }));
		expect(onTicketRemove).toHaveBeenCalledExactlyOnceWith('99', 12);
		s.stop();
	});

	it('ignores the keep-alive ping, unknown types, non-string and malformed frames', () => {
		const onInvalidate = vi.fn();
		const onActionResult = vi.fn();
		const s = createStream({ onInvalidate, onActionResult });
		s.start();
		const sock = FakeWebSocket.last();
		sock.open();

		sock.message(JSON.stringify({ type: 'ping' })); // keep-alive
		sock.message(JSON.stringify({ type: 'something_else' }));
		sock.message('not json at all');
		sock.message(new ArrayBuffer(4)); // non-string

		expect(onInvalidate).not.toHaveBeenCalled();
		expect(onActionResult).not.toHaveBeenCalled();
		s.stop();
	});
});

describe('createStream — reconnect with exponential backoff', () => {
	it('reconnects with widening 1s/2s/4s delays and signals onDisconnect', () => {
		const onConnect = vi.fn();
		const onDisconnect = vi.fn();
		const s = createStream({ onConnect, onDisconnect });
		s.start();
		expect(FakeWebSocket.instances).toHaveLength(1);

		// 1st drop -> reconnect after 1s
		FakeWebSocket.last().drop();
		expect(onDisconnect).toHaveBeenCalledTimes(1);
		vi.advanceTimersByTime(999);
		expect(FakeWebSocket.instances).toHaveLength(1);
		vi.advanceTimersByTime(1);
		expect(FakeWebSocket.instances).toHaveLength(2);

		// 2nd drop -> reconnect after 2s
		FakeWebSocket.last().drop();
		vi.advanceTimersByTime(1999);
		expect(FakeWebSocket.instances).toHaveLength(2);
		vi.advanceTimersByTime(1);
		expect(FakeWebSocket.instances).toHaveLength(3);

		// 3rd drop -> reconnect after 4s
		FakeWebSocket.last().drop();
		vi.advanceTimersByTime(4000);
		expect(FakeWebSocket.instances).toHaveLength(4);

		s.stop();
	});

	it('resets the backoff after a successful open', () => {
		const s = createStream({});
		s.start();

		// drop -> 1s -> reconnect (#2)
		FakeWebSocket.last().drop();
		vi.advanceTimersByTime(1000);
		expect(FakeWebSocket.instances).toHaveLength(2);

		// successful open resets the counter...
		FakeWebSocket.last().open();
		// ...so the next drop reconnects after 1s again (not 2s).
		FakeWebSocket.last().drop();
		vi.advanceTimersByTime(999);
		expect(FakeWebSocket.instances).toHaveLength(2);
		vi.advanceTimersByTime(1);
		expect(FakeWebSocket.instances).toHaveLength(3);

		s.stop();
	});

	it('stop() cancels any pending reconnect', () => {
		const s = createStream({});
		s.start();
		FakeWebSocket.last().drop(); // schedules a reconnect
		s.stop();
		vi.advanceTimersByTime(60_000);
		expect(FakeWebSocket.instances).toHaveLength(1); // no new socket
	});
});
