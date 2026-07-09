<!-- Small calm pill mapping a TicketStatus to its semantic colour pair.
     The backend should only ever send the three known values; an unexpected
     string degrades to a neutral pill rather than breaking. -->
<script lang="ts">
	import type { TicketStatus } from '$lib/api/types.js';

	let { status }: { status: TicketStatus | string } = $props();

	/** label + colour-token key for each known status; unknown falls through. */
	const META: Record<string, { label: string; key: string }> = {
		New: { label: 'New', key: 'new' },
		InProgress: { label: 'In Progress', key: 'progress' },
		Closed: { label: 'Closed', key: 'closed' }
	};

	const meta = $derived(META[status] ?? { label: status || 'Unknown', key: 'unknown' });
</script>

<span class="badge" data-status={meta.key}>{meta.label}</span>

<style>
	.badge {
		display: inline-block;
		padding: 0.1rem var(--space-2);
		border-radius: var(--radius-sm);
		font-size: 0.72rem;
		font-weight: 600;
		letter-spacing: 0.04em;
		white-space: nowrap;
		line-height: 1.4;
		/* default = unknown */
		color: var(--st-unknown-fg);
		background: var(--st-unknown-bg);
	}

	.badge[data-status='new'] {
		color: var(--st-new-fg);
		background: var(--st-new-bg);
	}
	.badge[data-status='progress'] {
		color: var(--st-progress-fg);
		background: var(--st-progress-bg);
	}
	.badge[data-status='closed'] {
		color: var(--st-closed-fg);
		background: var(--st-closed-bg);
	}
</style>
