<!-- The ticket table. Renders dashboard.tickets in the EXACT order the backend
     gives them — never re-sorted. Handles the loading / empty / error states. -->
<script lang="ts">
	import { dashboard } from '$lib/stores/dashboard.svelte';
	import TicketRow from './TicketRow.svelte';

	// First-load skeleton: loading and we have nothing to show yet.
	const showSkeleton = $derived(dashboard.loading && !dashboard.initialized);
	// Empty: a load has completed and there are simply no tickets.
	const showEmpty = $derived(dashboard.initialized && dashboard.tickets.length === 0);

	const SKELETON_ROWS = [0, 1, 2, 3, 4];
</script>

<section class="list" aria-label="Tickets">
	<div class="header" role="row">
		<!-- Disclosure column: kept in grid flow (an empty cell) so the header
		     strip stays aligned with the rows. A `.sr-only` span here would be
		     position:absolute and collapse the column. -->
		<span role="columnheader" aria-label="Expand" class="col-spacer"></span>
		<span role="columnheader">Status</span>
		<span role="columnheader">Subject</span>
		<span role="columnheader" class="col-phone">Caller</span>
		<span role="columnheader" class="col-assignee">Assignee</span>
		<span role="columnheader" class="sr-only">Open</span>
	</div>

	{#if dashboard.error}
		<p class="banner error" role="alert">
			<svg width="15" height="15" viewBox="0 0 24 24" fill="none" aria-hidden="true">
				<path
					d="M12 8.5v4.2M12 16.2v.1M10.3 3.9 2.6 17.4A2 2 0 0 0 4.3 20.5h15.4a2 2 0 0 0 1.7-3.1L13.7 3.9a2 2 0 0 0-3.4 0Z"
					stroke="currentColor"
					stroke-width="1.8"
					stroke-linecap="round"
					stroke-linejoin="round"
				/>
			</svg>
			<span>{dashboard.error}</span>
		</p>
	{/if}

	{#if showSkeleton}
		<div class="skeletons" aria-hidden="true">
			{#each SKELETON_ROWS as i (i)}
				<div class="srow" style="animation-delay: {i * 80}ms">
					<span class="sk sk-chevron"></span>
					<span class="sk sk-badge"></span>
					<span class="sk sk-line"></span>
					<span class="sk sk-mono col-phone"></span>
					<span class="sk sk-assignee col-assignee"></span>
					<span class="sk sk-icon"></span>
				</div>
			{/each}
		</div>
	{:else if showEmpty}
		<div class="empty">
			<span class="empty-glyph" aria-hidden="true">
				<svg width="34" height="34" viewBox="0 0 24 24" fill="none">
					<path
						d="M3 13h4l1.5 3h7L17 13h4M5 5h14a2 2 0 0 1 2 2v10a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V7a2 2 0 0 1 2-2Z"
						stroke="currentColor"
						stroke-width="1.6"
						stroke-linecap="round"
						stroke-linejoin="round"
					/>
				</svg>
			</span>
			<p class="empty-title">No tickets</p>
			<p class="empty-sub">Open call tickets will appear here as they arrive.</p>
		</div>
	{:else}
		<div class="rows">
			{#each dashboard.tickets as entry, i (entry.id)}
				<TicketRow {entry} index={i} />
			{/each}
		</div>
	{/if}
</section>

<style>
	.list {
		/* Shared column template — every row + the header read this. The leading
		   2rem column is the disclosure (expand) toggle. */
		--ticket-grid: 2rem 6.5rem minmax(0, 1fr) 11rem 8.5rem 2rem;
		border: 1px solid var(--color-border);
		border-radius: var(--radius-md);
		overflow: hidden;
		background: var(--color-surface);
		box-shadow: var(--shadow-sm);
	}

	.header {
		display: grid;
		grid-template-columns: var(--ticket-grid);
		gap: var(--space-4);
		padding: var(--space-2) var(--space-4);
		border-bottom: 1px solid var(--color-border);
		background: var(--color-surface-2);
		font-size: var(--text-xs);
		font-weight: 600;
		letter-spacing: var(--track-wide);
		text-transform: uppercase;
		color: var(--color-text-muted);
	}

	.banner.error {
		display: flex;
		align-items: center;
		gap: var(--space-2);
		margin: 0;
		padding: var(--space-3) var(--space-4);
		font-size: var(--text-sm);
		color: var(--st-rejected-fg);
		background: var(--st-rejected-bg);
		border-bottom: 1px solid var(--color-border);
	}
	.banner.error svg {
		flex: none;
	}

	/* — Skeleton — */
	.srow {
		display: grid;
		grid-template-columns: var(--ticket-grid);
		align-items: center;
		gap: var(--space-4);
		padding: var(--space-3) var(--space-4);
		border-bottom: 1px solid var(--color-border);
		animation: fade 1.2s ease-in-out infinite;
	}
	.sk {
		height: 0.85rem;
		border-radius: var(--radius-sm);
		background: linear-gradient(
			90deg,
			var(--skeleton-1) 25%,
			var(--skeleton-2) 50%,
			var(--skeleton-1) 75%
		);
		background-size: 200% 100%;
		animation: shimmer 1.4s linear infinite;
	}
	.sk-chevron {
		width: 0.9rem;
		height: 0.9rem;
	}
	.sk-badge {
		width: 3.5rem;
		height: 1rem;
	}
	.sk-line {
		width: 70%;
	}
	.sk-mono {
		width: 6rem;
	}
	.sk-assignee {
		width: 4.5rem;
	}
	.sk-icon {
		width: 1.25rem;
		justify-self: end;
	}

	@keyframes shimmer {
		0% {
			background-position: 200% 0;
		}
		100% {
			background-position: -200% 0;
		}
	}
	@keyframes fade {
		0%,
		100% {
			opacity: 1;
		}
		50% {
			opacity: 0.7;
		}
	}

	/* — Empty — */
	.empty {
		display: flex;
		flex-direction: column;
		align-items: center;
		padding: var(--space-6) var(--space-4);
		text-align: center;
	}
	.empty-glyph {
		display: inline-flex;
		margin-bottom: var(--space-3);
		color: var(--color-text-faint);
	}
	.empty-title {
		font-weight: 600;
		color: var(--color-text);
	}
	.empty-sub {
		margin-top: var(--space-1);
		font-size: var(--text-base);
		color: var(--color-text-muted);
	}

	.sr-only {
		position: absolute;
		width: 1px;
		height: 1px;
		padding: 0;
		margin: -1px;
		overflow: hidden;
		clip: rect(0, 0, 0, 0);
		white-space: nowrap;
		border: 0;
	}

	/* — Narrow operator window: drop Caller + Assignee to a secondary line that
	     TicketRow renders, and shrink the grid to 4 columns so nothing scrolls. — */
	@media (max-width: 34rem) {
		.list {
			--ticket-grid: 1.75rem 5.5rem minmax(0, 1fr) 1.75rem;
		}
		.col-phone,
		.col-assignee {
			display: none;
		}
		.header {
			gap: var(--space-3);
		}
	}

	@media (prefers-reduced-motion: reduce) {
		.sk,
		.srow {
			animation: none;
		}
	}
</style>
