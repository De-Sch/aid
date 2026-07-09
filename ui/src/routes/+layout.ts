// Applies to every route. The dashboard lives entirely behind a session cookie
// and is fully dynamic, so there is nothing to server-render or prerender:
// adapter-static emits a single SPA shell (index.html) and the client router
// takes over.
export const ssr = false;
export const prerender = false;

import { redirect } from '@sveltejs/kit';
import { session } from '$lib/stores/session.svelte';
import type { LayoutLoad } from './$types';

// Auth gate. Runs in the browser (ssr=false) on first load and every navigation,
// so the session cookie is present and the backend slides its expiry each time.
// whoami decides the redirect; `/login` and `/reset` are the only routes
// reachable logged-out (the latter is reached via a recovery-key grant cookie).
const PUBLIC_ROUTES = new Set(['/login', '/reset']);

export const load: LayoutLoad = async ({ url }) => {
	const authed = await session.refreshWhoami();
	const onPublic = PUBLIC_ROUTES.has(url.pathname);

	if (!authed && !onPublic) throw redirect(307, '/login');
	if (authed && onPublic) throw redirect(307, '/');

	return { username: session.username };
};
