/**
 * Display formatters for the dashboard.
 *
 * The backend sends call timestamps as the daemon's LOCAL wall-clock string
 * "YYYY-MM-DD HH:MM:SS" — the exact value also stored in OpenProject. We show
 * it verbatim (trimmed to the minute); we deliberately do NOT re-parse it as a
 * Date or apply any timezone math, which would double-convert. The timezone is
 * the daemon machine's (set per-deployment), so it is never hardcoded here.
 */

/**
 * Trim a "YYYY-MM-DD HH:MM:SS" local timestamp to "YYYY-MM-DD HH:MM" for
 * display. Returns "" for null/empty/short input so callers can render it
 * inline without guarding.
 *
 * @param {string | null | undefined} local
 * @returns {string}
 */
export function formatLocalDateTime(local) {
	if (!local) {
		return '';
	}
	// "YYYY-MM-DD HH:MM:SS" → "YYYY-MM-DD HH:MM"; leave anything unexpected as-is.
	return local.length >= 16 ? local.slice(0, 16) : local;
}

/**
 * Split a ticket's `description` into the rows the comment view shows, so the
 * dashboard reads 1:1 with the OpenProject work package.
 *
 * The description is markdown shared with OpenProject, whose renderer treats a
 * bare newline as a line break (hardbreaks). Markdown has no way to spell an
 * empty line, so OpenProject's own editor writes a literal `<br>` for one — and
 * that is the only thing this function has to interpret:
 *
 *   - `<br>` on its own line  → an empty row, i.e. the empty line OpenProject
 *     shows and lets you put the cursor in. Never the visible text "<br>".
 *   - a bare newline          → the next row, tight, no gap.
 *   - a blank source line     → dropped. It is markdown's paragraph separator,
 *     not content; OpenProject renders it as paragraph spacing, which the row
 *     gap already provides.
 *
 * Returns plain strings that the caller interpolates as text — never `{@html}`,
 * so text authored in OpenProject cannot inject markup.
 *
 * @param {string | null | undefined} description
 * @returns {string[]} rows, oldest first; `''` entries are empty lines
 */
export function commentDisplayLines(description) {
	if (!description) {
		return [];
	}

	const BR_ONLY = /^(?:<br\s*\/?>)+$/i;
	/** @type {string[]} */
	const rows = [];

	for (const raw of description.replace(/\r\n?/g, '\n').split('\n')) {
		const line = raw.trim();
		if (line.length === 0) {
			continue;
		}
		if (BR_ONLY.test(line)) {
			// One empty row per marker, so two stacked <br> stay two empty lines.
			for (const _ of line.match(/<br\s*\/?>/gi) ?? ['']) {
				rows.push('');
			}
			continue;
		}
		// A stray trailing/inline marker on a content line: the newline already
		// ends the row, so drop the marker rather than printing it.
		rows.push(line.replace(/<br\s*\/?>/gi, '').trimEnd());
	}

	// A leading or trailing empty row is padding around the content, not an
	// empty line the author typed.
	while (rows.length > 0 && rows[0] === '') {
		rows.shift();
	}
	while (rows.length > 0 && rows[rows.length - 1] === '') {
		rows.pop();
	}
	return rows;
}
