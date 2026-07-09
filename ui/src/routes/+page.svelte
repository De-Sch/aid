<!-- Operator dashboard: the active-call spotlight (the focal panel while the
     phone rings) above the live ticket list. Health lives in the header pill;
     toasts are mounted globally in the layout. -->
<script lang="ts">
	import { dashboard } from '$lib/stores/dashboard.svelte';
	import ActiveCallSpotlight from '$lib/components/ActiveCallSpotlight.svelte';
	import TicketList from '$lib/components/TicketList.svelte';

	// Start live data on mount; tear it down on unmount. (Health is polled by the
	// HealthPill in the header; this page only drives the dashboard stream.)
	$effect(() => {
		dashboard.start();
		return () => dashboard.stop();
	});

	// A genuine disconnect: we had data, then the stream dropped. (Distinct from
	// the initial connect, where `initialized` is still false and the list shows
	// its skeleton.)
	const offline = $derived(dashboard.initialized && !dashboard.connected);
</script>

<div class="page">
	<header class="page-head">
		<h1>Tickets</h1>
		<span class="conn" class:on={dashboard.connected}>
			<span class="conn-dot"></span>
			{dashboard.connected ? 'Live' : 'Reconnecting…'}
		</span>
	</header>

	{#if offline}
		<div class="offline" role="status" aria-live="polite">
			<span class="offline-glyph" aria-hidden="true"></span>
			<span>Connection lost — reconnecting. Showing the last known state.</span>
		</div>
	{/if}

	<ActiveCallSpotlight />

	<TicketList />
</div>

<style>
	.page {
		display: flex;
		flex-direction: column;
		gap: var(--space-4);
		max-width: 64rem;
		margin: 0 auto;
	}

	.page-head {
		display: flex;
		align-items: baseline;
		gap: var(--space-3);
	}
	.page-head h1 {
		font-size: var(--text-lg);
		font-weight: 600;
		letter-spacing: var(--track-tight);
	}

	.conn {
		display: inline-flex;
		align-items: center;
		gap: var(--space-1);
		font-size: var(--text-sm);
		color: var(--color-text-muted);
	}
	.conn-dot {
		width: 0.45rem;
		height: 0.45rem;
		border-radius: 50%;
		background: var(--health-idle);
	}
	.conn.on .conn-dot {
		background: var(--health-ok);
	}

	/* — Offline / reconnecting banner — calm, not alarming. — */
	.offline {
		display: flex;
		align-items: center;
		gap: var(--space-3);
		padding: var(--space-3) var(--space-4);
		border: 1px solid var(--color-border);
		border-radius: var(--radius-md);
		background: var(--color-surface-2);
		color: var(--color-text-muted);
		font-size: var(--text-base);
	}
	.offline-glyph {
		flex: none;
		width: 0.6rem;
		height: 0.6rem;
		border-radius: 50%;
		border: 2px solid var(--health-warn);
		border-top-color: transparent;
		animation: spin 0.9s linear infinite;
	}

	@keyframes spin {
		to {
			transform: rotate(360deg);
		}
	}

	@media (prefers-reduced-motion: reduce) {
		.offline-glyph {
			animation: none;
			border-top-color: var(--health-warn);
			opacity: 0.6;
		}
	}
</style>
