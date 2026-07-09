import { describe, it, expect } from 'vitest';
import { formatLocalDateTime } from './format.js';

describe('formatLocalDateTime', () => {
	it('returns "" for null / undefined / empty', () => {
		expect(formatLocalDateTime(null)).toBe('');
		expect(formatLocalDateTime(undefined)).toBe('');
		expect(formatLocalDateTime('')).toBe('');
	});

	it('trims a "YYYY-MM-DD HH:MM:SS" local timestamp to the minute', () => {
		expect(formatLocalDateTime('2026-06-08 15:44:57')).toBe('2026-06-08 15:44');
	});

	it('does NOT apply any timezone conversion (shows the daemon-local value verbatim)', () => {
		// The string is already the daemon's local wall-clock; re-parsing it as a
		// Date and localizing would shift it. We must show exactly what the
		// backend / OpenProject stored.
		expect(formatLocalDateTime('2026-06-08 15:44:57')).toBe('2026-06-08 15:44');
		expect(formatLocalDateTime('2026-12-31 23:59:00')).toBe('2026-12-31 23:59');
	});

	it('passes short / unexpected input through unchanged', () => {
		expect(formatLocalDateTime('2026-06-08')).toBe('2026-06-08');
	});
});
