/**
 * Session state — the single source of truth for "who is logged in".
 *
 * Backed entirely by `client.js`; this module owns no transport logic, only the
 * reactive state and the three actions the UI needs (whoami / login / logout).
 * Runes module (`.svelte.ts`): the `$state` below is shared app-wide, so every
 * component that reads `session.username` re-renders when it changes.
 *
 * The session itself lives in an HttpOnly cookie the browser sends automatically
 * — there is no token to hold here, only the resolved
 * username and the in-flight/error flags.
 */
import { client, ApiError, AuthError } from '$lib/api/client.js';

let username = $state<string | null>(null);
let loading = $state(false);
let error = $state<string | null>(null);

export const session = {
	/** Resolved username from the session cookie, or null when logged out. */
	get username(): string | null {
		return username;
	},
	/** True while a session request (whoami/login/logout) is in flight. */
	get loading(): boolean {
		return loading;
	},
	/** Generic, non-auth error message (e.g. server unreachable), or null. */
	get error(): string | null {
		return error;
	},
	/** Convenience: whether a username is currently resolved. */
	get isAuthenticated(): boolean {
		return username !== null;
	},

	/**
	 * Ask the backend who the cookie belongs to. Used by the layout gate on every
	 * navigation (which also slides the backend session expiry). Never throws.
	 * @returns true if authenticated, false otherwise.
	 */
	async refreshWhoami(): Promise<boolean> {
		loading = true;
		error = null;
		try {
			const who = await client.whoami();
			username = who.username;
			return true;
		} catch (e) {
			username = null;
			// 401 is the expected "not logged in" answer — not an error to surface.
			if (!(e instanceof AuthError)) {
				error = 'Could not reach the server. Please try again.';
			}
			return false;
		} finally {
			loading = false;
		}
	},

	/**
	 * Log in, then resolve the username so the UI is ready before navigating.
	 * Re-throws the original {@link ApiError} (AuthError 401 / RateLimitError 429 /
	 * …) so the caller can map it to an inline message.
	 *
	 * Returns `true` when the backend asked for a password reset instead of
	 * logging in — i.e. the supplied password was the recovery key. In that
	 * case no session exists yet (an `aid_reset` grant cookie was set instead),
	 * so the caller should navigate to `/reset` rather than the dashboard.
	 * @param {string} u
	 * @param {string} p
	 * @returns true if a reset is required, false on a normal login.
	 */
	async login(u: string, p: string): Promise<boolean> {
		loading = true;
		error = null;
		try {
			const res = await client.login(u, p);
			if (res && res.resetRequired) {
				// Recovery key accepted: a grant cookie is set, no session.
				username = null;
				return true;
			}
			const who = await client.whoami();
			username = who.username;
			return false;
		} catch (e) {
			username = null;
			throw e;
		} finally {
			loading = false;
		}
	},

	/**
	 * Set a new password using the single-use `aid_reset` grant cookie from a
	 * recovery-key login. Re-throws the original {@link ApiError} so the caller
	 * can map it to an inline message. On success the user has no session and
	 * must log in fresh.
	 * @param {string} newPassword
	 */
	async resetPassword(newPassword: string): Promise<void> {
		loading = true;
		error = null;
		try {
			await client.resetPassword(newPassword);
			username = null;
		} catch (e) {
			throw e;
		} finally {
			loading = false;
		}
	},

	/**
	 * Log out. Clears local state even if the request fails, so the UI never gets
	 * stuck "logged in" against a dead/asleep backend.
	 */
	async logout(): Promise<void> {
		loading = true;
		try {
			await client.logout();
		} catch {
			// Swallow: the cookie is server-side and the gate will re-check anyway.
		} finally {
			username = null;
			loading = false;
		}
	}
};

// Re-export for callers that want `instanceof` checks without a second import.
export { ApiError, AuthError };
