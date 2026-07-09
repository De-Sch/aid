import adapter from '@sveltejs/adapter-static';

/** @type {import('@sveltejs/kit').Config} */
const config = {
	compilerOptions: {
		// Force runes mode for the project, except for libraries. Can be removed in svelte 6.
		runes: ({ filename }) => filename.split(/[/\\]/).includes('node_modules') ? undefined : true
	},
	// adapter-static in SPA mode: every route falls back to index.html so the
	// client-side router (behind the session cookie) owns all navigation.
	kit: { adapter: adapter({ fallback: 'index.html' }) }
};

export default config;
