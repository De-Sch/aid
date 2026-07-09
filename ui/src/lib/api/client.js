/**
 * Typed fetch client for the AID2.0 dashboard.
 *
 * The single place that talks to the backend. Every screen calls these methods
 * and never re-implements HTTP status handling. Same-origin requests with the
 * session cookie always attached (`credentials: 'include'`).
 *
 * Error model (centralized here):
 *   - 401                -> AuthError       (caller redirects to /login; login page reads it as bad creds)
 *   - 429                -> RateLimitError  (login throttle)
 *   - other 4xx + {error}-> ProtocolError   (generic "invalid request")
 *   - 5xx  + {error}     -> ServerError     (generic failure)
 *   - 2xx (incl. {ok:false}) -> the parsed body is RETURNED as-is; a business
 *                               failure is data (ActionResult.ok === false), not an exception.
 */

/**
 * Base class for every HTTP error this client throws. Catch this for a catch-all,
 * or one of the subclasses (by name / instanceof) for specific handling.
 */
export class ApiError extends Error {
	/**
	 * @param {string} name
	 * @param {string} message       Human-facing generic message (no secrets).
	 * @param {number} status        HTTP status code.
	 * @param {string | null} serverError  Raw `{error}` text from the backend, or null.
	 */
	constructor(name, message, status, serverError) {
		super(message);
		this.name = name;
		this.status = status;
		this.serverError = serverError;
	}
}

/** 401 — session expired or absent. Callers redirect to /login. */
export class AuthError extends ApiError {
	/** @param {string | null} [serverError] */
	constructor(serverError = null) {
		super('AuthError', 'session expired or unauthenticated', 401, serverError);
	}
}

/** 429 — login attempts rate-limited. */
export class RateLimitError extends ApiError {
	/** @param {string | null} [serverError] */
	constructor(serverError = null) {
		super('RateLimitError', 'too many attempts, try again in a moment', 429, serverError);
	}
}

/** Other 4xx — bad request / bad id / bad JSON. Show a generic "invalid request". */
export class ProtocolError extends ApiError {
	/**
	 * @param {string | null} [serverError]
	 * @param {number} [status]
	 */
	constructor(serverError = null, status = 400) {
		super('ProtocolError', 'invalid request', status, serverError);
	}
}

/** 5xx — backend failure. Show a generic failure toast. */
export class ServerError extends ApiError {
	/**
	 * @param {string | null} [serverError]
	 * @param {number} [status]
	 */
	constructor(serverError = null, status = 500) {
		super('ServerError', 'internal error', status, serverError);
	}
}

/**
 * Map a thrown error to a short, human-facing line for inline display. Every
 * ApiError already carries a safe generic `.message`; anything else (a bug, a
 * network failure) degrades to a neutral fallback. Note: AuthError is usually
 * handled by the caller (redirect to /login) before reaching here.
 * @param {unknown} e
 * @returns {string}
 */
export function describeError(e) {
	return e instanceof ApiError ? e.message : 'Something went wrong. Please try again.';
}

/**
 * Parse a response body as JSON, tolerating empty/non-JSON bodies (returns null).
 * @param {Response} res
 * @returns {Promise<any>}
 */
async function parseJson(res) {
	const text = await res.text();
	if (!text) return null;
	try {
		return JSON.parse(text);
	} catch {
		return null;
	}
}

/**
 * Core request: attaches the cookie, JSON-encodes the body, parses the response,
 * and maps non-2xx status codes onto the error classes above.
 * @param {string} path
 * @param {{ method?: string, body?: unknown }} [opts]
 * @returns {Promise<any>} parsed 2xx JSON body
 */
async function request(path, opts = {}) {
	const { method = 'GET', body } = opts;
	/** @type {RequestInit} */
	const init = { method, credentials: 'include', headers: {} };
	if (body !== undefined) {
		init.headers = { 'Content-Type': 'application/json' };
		init.body = JSON.stringify(body);
	}

	const res = await fetch(path, init);
	const data = await parseJson(res);

	if (res.ok) return data; // 2xx — includes ActionResult with ok:false

	const serverError = data && typeof data.error === 'string' ? data.error : null;
	if (res.status === 401) throw new AuthError(serverError);
	if (res.status === 429) throw new RateLimitError(serverError);
	if (res.status >= 400 && res.status < 500) throw new ProtocolError(serverError, res.status);
	throw new ServerError(serverError, res.status);
}

/**
 * Dev-only structural sanity check on the dashboard payload. Throws loudly if the
 * backend shape drifts. Compiled out of meaning in prod (gated on import.meta.env.DEV),
 * so there is zero production cost and no runtime schema dependency.
 * @param {any} d
 */
function assertDashboardShape(d) {
	if (
		!d ||
		typeof d !== 'object' ||
		!Array.isArray(d.tickets) ||
		!('active' in d) ||
		!('addressCallInformation' in d)
	) {
		throw new Error('getDashboard: unexpected payload shape (backend drift?): ' + JSON.stringify(d));
	}
}

const isDev = typeof import.meta.env !== 'undefined' && import.meta.env.DEV;

/**
 * @param {string} id ticket id
 * @returns {string} path-safe segment
 */
const idSeg = (id) => encodeURIComponent(String(id));

export const client = {
	/**
	 * POST /ui/login — sets the HttpOnly session cookie on a normal login,
	 * or an `aid_reset` grant cookie + `{resetRequired:true}` when the
	 * password was the recovery key.
	 * @param {string} username
	 * @param {string} password
	 * @returns {Promise<import('./types.js').LoginAck>}
	 */
	login(username, password) {
		return request('/ui/login', { method: 'POST', body: { username, password } });
	},

	/**
	 * POST /ui/reset — set a new password using the single-use `aid_reset`
	 * grant cookie minted by a recovery-key login. The username is bound to
	 * the grant server-side, so only the new password is sent. Throws
	 * AuthError (401) if the grant is missing/expired, ProtocolError (400)
	 * if the password is too weak.
	 * @param {string} newPassword
	 * @returns {Promise<import('./types.js').OkAck>}
	 */
	resetPassword(newPassword) {
		return request('/ui/reset', { method: 'POST', body: { newPassword } });
	},

	/**
	 * POST /ui/logout — clears the session cookie. Idempotent.
	 * @returns {Promise<import('./types.js').OkAck>}
	 */
	logout() {
		return request('/ui/logout', { method: 'POST' });
	},

	/**
	 * GET /ui/whoami — who the cookie belongs to. Throws AuthError if not logged in.
	 * @returns {Promise<import('./types.js').WhoAmI>}
	 */
	whoami() {
		return request('/ui/whoami');
	},

	/**
	 * GET /ui/dashboard — the single source of truth for the dashboard.
	 * @returns {Promise<import('./types.js').DashboardView>}
	 */
	async getDashboard() {
		const data = await request('/ui/dashboard');
		if (isDev) assertDashboardShape(data);
		return data;
	},

	/**
	 * POST /ui/comment/{id} — append a comment. Returns ActionResult (ok may be false).
	 * @param {string} id ticket id
	 * @param {string} comment
	 * @returns {Promise<import('./types.js').ActionResult>}
	 */
	postComment(id, comment) {
		return request(`/ui/comment/${idSeg(id)}`, { method: 'POST', body: { comment } });
	},

	/**
	 * POST /ui/close/{id} — close a ticket (two-step status flow server-side, no body).
	 * @param {string} id ticket id
	 * @returns {Promise<import('./types.js').ActionResult>}
	 */
	closeTicket(id) {
		return request(`/ui/close/${idSeg(id)}`, { method: 'POST' });
	},

	/**
	 * GET /health — status pill data (no auth required).
	 * @returns {Promise<import('./types.js').Health>}
	 */
	getHealth() {
		return request('/health');
	}
};

export default client;
