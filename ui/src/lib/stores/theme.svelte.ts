/**
 * Theme preference — light / dark / auto (follow the OS).
 *
 * The choice is persisted in localStorage and reflected onto <html data-theme>,
 * which app.css reads to pick the token set. An inline snippet in app.html applies
 * the stored value before first paint to avoid a light→dark flash; this store keeps
 * the document in sync for the rest of the session and exposes the current mode to
 * the header toggle.
 *
 *   auto  → no data-theme attribute (CSS falls back to prefers-color-scheme)
 *   light → data-theme="light"
 *   dark  → data-theme="dark"
 *
 * Runes module (`.svelte.ts`): the `$state` below is shared app-wide.
 */
import { browser } from '$app/environment';

export type ThemeMode = 'light' | 'dark' | 'auto';

const STORAGE_KEY = 'aid-theme';
const ORDER: ThemeMode[] = ['auto', 'light', 'dark'];

function read(): ThemeMode {
	if (!browser) return 'auto';
	const v = localStorage.getItem(STORAGE_KEY);
	return v === 'light' || v === 'dark' ? v : 'auto';
}

/** Push the mode onto <html> + storage. 'auto' removes the attribute. */
function apply(m: ThemeMode): void {
	if (!browser) return;
	const root = document.documentElement;
	if (m === 'auto') {
		root.removeAttribute('data-theme');
		localStorage.removeItem(STORAGE_KEY);
	} else {
		root.dataset.theme = m;
		localStorage.setItem(STORAGE_KEY, m);
	}
}

let mode = $state<ThemeMode>(read());

export const theme = {
	/** The operator's current preference. */
	get mode(): ThemeMode {
		return mode;
	},

	/** Set an explicit mode. */
	set(m: ThemeMode): void {
		mode = m;
		apply(m);
	},

	/** Advance auto → light → dark → auto. Drives the header toggle. */
	cycle(): void {
		const next = ORDER[(ORDER.indexOf(mode) + 1) % ORDER.length];
		theme.set(next);
	}
};
