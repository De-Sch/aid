import { describe, it, expect } from 'vitest';
import { compareEntries, upsertEntry, removeEntry, deriveActive } from './dashboard-merge.js';

/**
 * Build a minimal-but-complete DashboardEntry. Only the fields the merge cares
 * about (id, status, updatedAt, lockVersion) vary per test; the rest are filler.
 * @param {Partial<import('../api/types.js').DashboardEntry>} over
 * @returns {import('../api/types.js').DashboardEntry}
 */
function entry(over) {
	return {
		id: '1',
		subject: 's',
		status: 'New',
		statusId: '1',
		callIds: [],
		callerNumber: '+49',
		calledNumber: null,
		assignee: null,
		callStart: null,
		callEnd: null,
		href: 'h',
		projectName: 'p',
		activeCallForViewer: null,
		otherActiveUsers: [],
		description: '',
		updatedAt: '2026-06-09T10:00:00Z',
		...over
	};
}

const ids = (/** @type {import('../api/types.js').DashboardEntry[]} */ list) => list.map((e) => e.id);

describe('compareEntries — server order (status rank → updatedAt desc → id asc)', () => {
	it('orders by status rank New<InProgress<Closed', () => {
		const list = [
			entry({ id: 'a', status: 'Closed' }),
			entry({ id: 'b', status: 'New' }),
			entry({ id: 'd', status: 'InProgress' })
		];
		expect(ids([...list].sort(compareEntries))).toEqual(['b', 'd', 'a']);
	});

	it('breaks status ties by updatedAt descending (newest first)', () => {
		const list = [
			entry({ id: 'old', status: 'New', updatedAt: '2026-06-09T08:00:00Z' }),
			entry({ id: 'new', status: 'New', updatedAt: '2026-06-09T12:00:00Z' }),
			entry({ id: 'mid', status: 'New', updatedAt: '2026-06-09T10:00:00Z' })
		];
		expect(ids([...list].sort(compareEntries))).toEqual(['new', 'mid', 'old']);
	});

	it('breaks updatedAt ties by id ascending (lexicographic)', () => {
		const t = '2026-06-09T10:00:00Z';
		const list = [
			entry({ id: '30', updatedAt: t }),
			entry({ id: '100', updatedAt: t }),
			entry({ id: '4', updatedAt: t })
		];
		// Lexicographic, matching the backend's id.v string compare.
		expect(ids([...list].sort(compareEntries))).toEqual(['100', '30', '4']);
	});
});

describe('upsertEntry', () => {
	it('inserts a brand-new row in sorted position', () => {
		const list = [
			entry({ id: 'a', status: 'New' }),
			entry({ id: 'b', status: 'Closed' })
		];
		const next = upsertEntry(list, entry({ id: 'c', status: 'InProgress', lockVersion: 1 }));
		expect(ids(next)).toEqual(['a', 'c', 'b']);
		expect(next).not.toBe(list); // new reference for reactivity
	});

	it('replaces an existing row when the incoming version is newer and re-sorts', () => {
		const list = [
			entry({ id: 'a', status: 'New', updatedAt: '2026-06-09T08:00:00Z', lockVersion: 1 }),
			entry({ id: 'b', status: 'New', updatedAt: '2026-06-09T09:00:00Z', lockVersion: 1 })
		];
		// 'a' updated → newest → should move ahead of 'b'.
		const next = upsertEntry(
			list,
			entry({ id: 'a', status: 'New', updatedAt: '2026-06-09T10:00:00Z', lockVersion: 2 })
		);
		expect(ids(next)).toEqual(['a', 'b']);
		expect(next[0].lockVersion).toBe(2);
	});

	it('moves a row to a new status group on a status change', () => {
		const list = [
			entry({ id: 'a', status: 'New', lockVersion: 1 }),
			entry({ id: 'b', status: 'New', lockVersion: 1 })
		];
		const next = upsertEntry(list, entry({ id: 'a', status: 'Closed', lockVersion: 2 }));
		expect(ids(next)).toEqual(['b', 'a']);
	});

	it('ignores a stale frame (lower or equal lockVersion) and keeps the same reference', () => {
		const list = [entry({ id: 'a', status: 'New', subject: 'fresh', lockVersion: 5 })];
		const stale = upsertEntry(list, entry({ id: 'a', subject: 'stale', lockVersion: 4 }));
		expect(stale).toBe(list);
		const equal = upsertEntry(list, entry({ id: 'a', subject: 'stale', lockVersion: 5 }));
		expect(equal).toBe(list);
		expect(list[0].subject).toBe('fresh');
	});

	it('applies an upsert over a REST-snapshot row that has no lockVersion', () => {
		const list = [entry({ id: 'a', status: 'New', subject: 'snapshot' })]; // no lockVersion
		const next = upsertEntry(list, entry({ id: 'a', subject: 'live', lockVersion: 1 }));
		expect(next[0].subject).toBe('live');
		expect(next[0].lockVersion).toBe(1);
	});
});

describe('removeEntry', () => {
	it('drops the row with the given id', () => {
		const list = [entry({ id: 'a', lockVersion: 1 }), entry({ id: 'b', lockVersion: 1 })];
		const next = removeEntry(list, 'a', 2);
		expect(ids(next)).toEqual(['b']);
	});

	it('is a no-op (same reference) when the id is absent', () => {
		const list = [entry({ id: 'a', lockVersion: 1 })];
		expect(removeEntry(list, 'zzz', 9)).toBe(list);
	});

	it('drops a REST-snapshot row that has no lockVersion', () => {
		const list = [entry({ id: 'a' })]; // no lockVersion
		expect(ids(removeEntry(list, 'a', 1))).toEqual([]);
	});

	it('ignores a stale remove when a strictly-newer upsert was already applied', () => {
		const list = [entry({ id: 'a', lockVersion: 5 })];
		const next = removeEntry(list, 'a', 4); // older than what we hold
		expect(next).toBe(list);
		expect(ids(next)).toEqual(['a']);
	});

	it('honors a remove whose version equals the held version', () => {
		const list = [entry({ id: 'a', lockVersion: 5 })];
		expect(ids(removeEntry(list, 'a', 5))).toEqual([]);
	});
});

describe('deriveActive', () => {
	it('returns null for an empty list', () => {
		expect(deriveActive([])).toBeNull();
	});

	it('returns null when no entry has an active call for the viewer', () => {
		const list = [entry({ id: 'a' }), entry({ id: 'b' })];
		expect(deriveActive(list)).toBeNull();
	});

	it('picks the first entry (in list/sort order) with an active call', () => {
		const list = [
			entry({ id: 'a' }),
			entry({ id: 'b', activeCallForViewer: 'call-1', callerNumber: '+1', projectName: 'p1' }),
			entry({ id: 'c', activeCallForViewer: 'call-2', callerNumber: '+2', projectName: 'p2' })
		];
		expect(deriveActive(list)).toEqual({
			ticketId: 'b',
			callId: 'call-1',
			projectName: 'p1',
			callerNumber: '+1'
		});
	});

	it('maps fields from the entry (id -> ticketId, activeCallForViewer -> callId)', () => {
		const list = [
			entry({ id: '42', activeCallForViewer: 'call-9', callerNumber: '+49 123', projectName: 'support' })
		];
		expect(deriveActive(list)).toEqual({
			ticketId: '42',
			callId: 'call-9',
			projectName: 'support',
			callerNumber: '+49 123'
		});
	});
});
