import { sveltekit } from '@sveltejs/kit/vite';
import { defineConfig } from 'vite';

// ── EDIT ME ────────────────────────────────────────────────────────────────
// The running AID2.0 daemon (LAN host:port). Dev requests to /ui and /health
// are proxied here so the browser sees a single origin (cookie + WebSocket
// behave exactly as in prod). localhost is a "secure context", so the daemon's
// Secure session cookie works over plain http with no config change.
// Override per-run with: AID_DAEMON=http://192.168.178.54:8088 pnpm dev
const DAEMON_TARGET = process.env.AID_DAEMON ?? 'http://127.0.0.1:8088';
// ─────────────────────────────────────────────────────────────────────────────

export default defineConfig({
	plugins: [sveltekit()],
	server: {
		port: 5173,
		proxy: {
			// REST + WebSocket. ws:true upgrades /ui/stream to a real socket.
			'/ui': { target: DAEMON_TARGET, changeOrigin: true, ws: true },
			// Unauthenticated health endpoint (status pill).
			'/health': { target: DAEMON_TARGET, changeOrigin: true }
		}
	}
});
