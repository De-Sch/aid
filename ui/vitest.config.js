import { defineConfig } from 'vitest/config';

// Unit tests run in plain Node — the API client is just `fetch`, no DOM needed.
// Kept separate from vite.config.ts so the dev/build pipeline stays untouched.
export default defineConfig({
	test: {
		environment: 'node',
		include: ['src/**/*.test.js']
	}
});
