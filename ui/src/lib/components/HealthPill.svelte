<!-- Live daemon health, shown as a coloured dot + short label with a detail
     popover on hover/focus. Owns its own poll lifecycle so it works wherever
     it is mounted (currently the app header). -->
<script lang="ts">
	import { health } from '$lib/stores/health.svelte';

	// Poll while mounted; stop on teardown. Both calls are idempotent.
	$effect(() => {
		health.start();
		return () => health.stop();
	});

	// Dot + label: green when "ok", amber when "degraded", grey otherwise
	// (starting, no data yet, or the daemon is unreachable).
	const tone = $derived(
		!health.reachable || !health.status
			? 'idle'
			: health.status === 'ok'
				? 'ok'
				: health.status === 'degraded'
					? 'warn'
					: 'idle'
	);

	const label = $derived(
		!health.reachable ? 'Unreachable' : (health.status ?? 'Starting')
	);

	/** Seconds → a compact human duration, e.g. "2d 3h", "5m 12s". */
	function formatUptime(s: number | null | undefined): string {
		if (s == null || s < 0) return '—';
		const d = Math.floor(s / 86400);
		const h = Math.floor((s % 86400) / 3600);
		const m = Math.floor((s % 3600) / 60);
		const sec = Math.floor(s % 60);
		if (d > 0) return `${d}d ${h}h`;
		if (h > 0) return `${h}h ${m}m`;
		if (m > 0) return `${m}m ${sec}s`;
		return `${sec}s`;
	}

	const d = $derived(health.data);
</script>

<div class="pill-wrap">
	<button type="button" class="pill" aria-haspopup="true" data-tone={tone}>
		<span class="dot" data-tone={tone}></span>
		<span class="label">{label}</span>
	</button>

	<div class="popover" role="tooltip">
		{#if d}
			<dl class="rows">
				<dt>Ticket system</dt>
				<dd data-reach={d.ticketSystem}>{d.ticketSystem}</dd>
				<dt>Address system</dt>
				<dd data-reach={d.addressSystem}>{d.addressSystem}</dd>
				<dt>Queued</dt>
				<dd class="mono">{d.queuedEvents}</dd>
				<dt>Failed</dt>
				<dd class="mono" data-bad={d.failedEvents > 0}>{d.failedEvents}</dd>
				<dt>Uptime</dt>
				<dd class="mono">{formatUptime(d.uptimeS)}</dd>
			</dl>
		{:else}
			<p class="waiting">Waiting for /health…</p>
		{/if}
	</div>
</div>

<style>
	.pill-wrap {
		position: relative;
		display: inline-flex;
	}

	.pill {
		display: inline-flex;
		align-items: center;
		gap: var(--space-2);
		padding: var(--space-1) var(--space-3);
		border: 1px solid var(--color-border);
		border-radius: 999px;
		background: var(--color-surface);
		font-size: 0.82rem;
		color: var(--color-text-muted);
		cursor: default;
	}

	.dot {
		width: 0.5rem;
		height: 0.5rem;
		border-radius: 50%;
		flex: none;
		background: var(--health-idle);
	}
	.dot[data-tone='ok'] {
		background: var(--health-ok);
	}
	.dot[data-tone='warn'] {
		background: var(--health-warn);
	}

	.label {
		text-transform: capitalize;
	}

	/* — Popover: hidden until the pill is hovered or focused within. — */
	.popover {
		position: absolute;
		top: calc(100% + var(--space-2));
		right: 0;
		z-index: 20;
		min-width: 13rem;
		padding: var(--space-3);
		background: var(--color-surface);
		border: 1px solid var(--color-border);
		border-radius: var(--radius-md);
		box-shadow: var(--shadow-md);
		opacity: 0;
		transform: translateY(-2px);
		pointer-events: none;
		transition:
			opacity 120ms ease,
			transform 120ms ease;
	}
	.pill-wrap:hover .popover,
	.pill-wrap:focus-within .popover {
		opacity: 1;
		transform: translateY(0);
		pointer-events: auto;
	}

	.rows {
		display: grid;
		grid-template-columns: auto 1fr;
		gap: var(--space-1) var(--space-4);
		margin: 0;
		font-size: 0.82rem;
	}
	.rows dt {
		color: var(--color-text-muted);
	}
	.rows dd {
		margin: 0;
		text-align: right;
		font-variant-numeric: tabular-nums;
	}

	dd[data-reach='unreachable'] {
		color: var(--st-rejected-fg);
	}
	dd[data-reach='reachable'] {
		color: var(--st-closed-fg);
	}
	dd[data-bad='true'] {
		color: var(--st-rejected-fg);
		font-weight: 600;
	}

	.waiting {
		margin: 0;
		font-size: 0.82rem;
		color: var(--color-text-muted);
	}
</style>
