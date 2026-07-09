import { describe, it, expect, vi, afterEach } from 'vitest';
import { client, AuthError, RateLimitError, ProtocolError, ServerError } from './client.js';

/**
 * Stub `global.fetch` with a minimal Response (the client only reads
 * `ok`, `status`, and `text()`).
 * @param {number} status
 * @param {unknown} [body]
 */
function mockFetch(status, body) {
	const text = body === undefined ? '' : JSON.stringify(body);
	const fn = vi.fn().mockResolvedValue({
		ok: status >= 200 && status < 300,
		status,
		text: async () => text
	});
	global.fetch = fn;
	return fn;
}

afterEach(() => {
	vi.restoreAllMocks();
});

describe('client error-status mapping', () => {
	it('401 -> AuthError (carrying the server error text)', async () => {
		mockFetch(401, { error: 'unauthenticated' });
		await expect(client.whoami()).rejects.toBeInstanceOf(AuthError);
		mockFetch(401, { error: 'unauthenticated' });
		await expect(client.whoami()).rejects.toMatchObject({ status: 401, serverError: 'unauthenticated' });
	});

	it('429 -> RateLimitError', async () => {
		mockFetch(429, { error: 'too many requests' });
		await expect(client.login('u', 'p')).rejects.toBeInstanceOf(RateLimitError);
	});

	it('other 4xx -> ProtocolError (preserving the status)', async () => {
		mockFetch(400, { error: 'bad request' });
		await expect(client.login('u', 'p')).rejects.toBeInstanceOf(ProtocolError);
		mockFetch(404, { error: 'not found' });
		await expect(client.closeTicket('42')).rejects.toMatchObject({ status: 404 });
	});

	it('5xx -> ServerError', async () => {
		mockFetch(500, { error: 'internal' });
		await expect(client.getHealth()).rejects.toBeInstanceOf(ServerError);
	});

	it('200 with {ok:false} returns the ActionResult as-is (business failure, no throw)', async () => {
		const result = { ok: false, op: 'COMMENT_SAVE', ticketId: '42', message: 'rejected' };
		mockFetch(200, result);
		await expect(client.postComment('42', 'hi')).resolves.toEqual(result);
	});

	it('attaches the cookie and JSON-encodes the body', async () => {
		const fn = mockFetch(200, { ok: true });
		await client.login('alice', 'secret');
		expect(fn).toHaveBeenCalledWith(
			'/ui/login',
			expect.objectContaining({
				method: 'POST',
				credentials: 'include',
				headers: { 'Content-Type': 'application/json' },
				body: JSON.stringify({ username: 'alice', password: 'secret' })
			})
		);
	});
});
