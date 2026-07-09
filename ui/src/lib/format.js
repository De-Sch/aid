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
