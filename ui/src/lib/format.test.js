import { describe, it, expect } from 'vitest';
import { formatLocalDateTime, commentDisplayLines } from './format.js';

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

describe('commentDisplayLines', () => {
	it('returns [] for null / undefined / empty', () => {
		expect(commentDisplayLines(null)).toEqual([]);
		expect(commentDisplayLines(undefined)).toEqual([]);
		expect(commentDisplayLines('')).toEqual([]);
	});

	// Unchanged from the original renderer: one row per line, blank source lines
	// (markdown's paragraph separator) dropped.
	it('gives one row per line and drops blank source lines', () => {
		expect(commentDisplayLines('one\ntwo')).toEqual(['one', 'two']);
		expect(commentDisplayLines('one\n\ntwo')).toEqual(['one', 'two']);
		expect(commentDisplayLines('  one  \n\n\n  two  ')).toEqual(['one', 'two']);
	});

	// The only behaviour change: "<br>" is an empty line, never visible text.
	it('renders a lone <br> as an empty row, not as text', () => {
		expect(commentDisplayLines('one\n\n<br>\n\ntwo')).toEqual(['one', '', 'two']);
		expect(commentDisplayLines('one\n<br>\ntwo')).toEqual(['one', '', 'two']);
	});

	it('never shows a <br> marker as text, in any spelling', () => {
		const out = commentDisplayLines('a\n<br>\nb\n<BR/>\nc\n<br />\nd');
		expect(out.join('')).not.toMatch(/br/i);
		expect(out).toEqual(['a', '', 'b', '', 'c', '', 'd']);
	});

	it('keeps two stacked markers as two empty rows', () => {
		expect(commentDisplayLines('a\n<br><br>\nb')).toEqual(['a', '', '', 'b']);
		expect(commentDisplayLines('a\n<br>\n<br>\nb')).toEqual(['a', '', '', 'b']);
	});

	it('strips a stray marker off a content line rather than printing it', () => {
		expect(commentDisplayLines('ohne line<br>\nnext')).toEqual(['ohne line', 'next']);
	});

	it('drops padding empty rows at the very start and end', () => {
		expect(commentDisplayLines('<br>\n\nonly\n\n<br>')).toEqual(['only']);
	});

	it('returns [] for a description that is nothing but markers', () => {
		expect(commentDisplayLines('<br>\n\n<br>')).toEqual([]);
	});

	it('normalises CRLF', () => {
		expect(commentDisplayLines('one\r\ntwo')).toEqual(['one', 'two']);
	});
});

// The point of the change: what the operator types, what OpenProject stores and
// renders, and what this view shows must be the same thing. Mirrors
// aid::domain::CommentText::toMarkdown (tests/domain/test_commenttext.cpp) and
// AppendComment's "\n" join. OpenProject's rendering of each stored form was
// measured against its own POST /api/v3/render/markdown.
describe('commentDisplayLines ↔ CommentText::toMarkdown round trip', () => {
	/** Mirror of aid::domain::CommentText::toMarkdown. */
	const toMarkdown = (/** @type {string} */ typed) =>
		typed.replace(/\r\n?/g, '\n').replace(/\n{2,}/g, (run) => '\n\n' + '<br>\n\n'.repeat(run.length - 1));
	/** Mirror of AppendComment's reducer. */
	const append = (/** @type {string} */ description, /** @type {string} */ typed) =>
		(description ? description + '\n' : '') + toMarkdown(typed);

	it('one Enter is one line break, with no empty line', () => {
		// OpenProject: <p>one<br>two</p>
		expect(commentDisplayLines(append('', 'one\ntwo'))).toEqual(['one', 'two']);
	});

	it('two Enters give exactly one empty line', () => {
		// OpenProject: <p>above</p><br><p>below</p> — a real, cursor-placeable line.
		expect(commentDisplayLines(append('', 'above\n\nbelow'))).toEqual(['above', '', 'below']);
	});

	it('three Enters give exactly two empty lines', () => {
		expect(commentDisplayLines(append('', 'a\n\n\nb'))).toEqual(['a', '', '', 'b']);
	});

	it('a new comment starts on the next line, with no empty line', () => {
		expect(commentDisplayLines(append('existing', 'reply'))).toEqual(['existing', 'reply']);
	});

	it('survives many appends without markers accumulating or drifting', () => {
		let d = '';
		for (const c of ['one', 'two\nstill two', 'three\n\nafter blank']) {
			d = append(d, c);
		}
		expect(commentDisplayLines(d)).toEqual([
			'one',
			'two',
			'still two',
			'three',
			'',
			'after blank'
		]);
	});

	it('interleaves cleanly with a comment typed in OpenProject’s own editor', () => {
		// OpenProject writes a lone "<br>" paragraph for its empty line.
		const fromOpenProject = 'op one\n\n<br>\n\nop two';
		expect(commentDisplayLines(append(fromOpenProject, 'dashboard reply'))).toEqual([
			'op one',
			'',
			'op two',
			'dashboard reply'
		]);
	});
});
