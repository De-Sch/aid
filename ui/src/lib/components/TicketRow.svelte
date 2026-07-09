<!-- One dense ticket row, expandable to reveal its actions. Columns are driven
     by --ticket-grid set on the list container so the header strip and every
     row stay aligned. A row whose activeCallForViewer is set gets a calm teal
     rail + slow pulse. The leading disclosure button toggles an actions panel
     (comment composer + close) below the row; its expanded state is local and
     survives a dashboard refetch because the list keys rows by id.

     On a narrow operator window the Caller/Assignee columns collapse into a
     secondary line (.subline) so the table never scrolls sideways. -->
<script lang="ts">
	import { slide } from 'svelte/transition';
	import { prefersReducedMotion } from 'svelte/motion';
	import type { DashboardEntry } from '$lib/api/types.js';
	import { formatLocalDateTime } from '$lib/format.js';
	import StatusBadge from './StatusBadge.svelte';
	import CommentThread from './CommentThread.svelte';
	import CommentComposer from './CommentComposer.svelte';
	import CloseButton from './CloseButton.svelte';

	let { entry, index = 0 }: { entry: DashboardEntry; index?: number } = $props();

	const live = $derived(entry.activeCallForViewer != null);
	// Call start rendered in the viewer's own timezone (backend sends UTC).
	const startedAt = $derived(formatLocalDateTime(entry.callStart));

	let expanded = $state(false);
	const panelId = $derived(`actions-${entry.id}`);
	const slideDuration = $derived(prefersReducedMotion.current ? 0 : 180);
	// Cap the entrance stagger so a long first load still settles quickly.
	const enterDelay = $derived(`${Math.min(index, 6) * 40}ms`);
</script>

<div class="ticket" class:live class:expanded style="--enter-delay: {enterDelay}">
	<!-- The whole row toggles the actions panel. Clicks that land on a nested
	     <a> (open-ticket link) or <button> (the disclosure chevron) are left
	     to those controls. Keyboard access goes through the .disclosure button,
	     which is why this static-element handler is safe to ignore the a11y lint. -->
	<!-- svelte-ignore a11y_click_events_have_key_events -->
	<!-- svelte-ignore a11y_no_static_element_interactions -->
	<div
		class="row"
		aria-current={live ? 'true' : undefined}
		onclick={(e) => {
			if (!(e.target instanceof Element) || !e.target.closest('a, button')) {
				expanded = !expanded;
			}
		}}
	>
		<span class="rail" aria-hidden="true"></span>

		<div class="cell disclosure-cell">
			<button
				type="button"
				class="disclosure"
				aria-expanded={expanded}
				aria-controls={panelId}
				aria-label={expanded ? `Hide actions for “${entry.subject}”` : `Show actions for “${entry.subject}”`}
				onclick={() => (expanded = !expanded)}
			>
				<svg
					class="chevron"
					width="14"
					height="14"
					viewBox="0 0 24 24"
					fill="none"
					aria-hidden="true"
				>
					<path
						d="M9 6l6 6-6 6"
						stroke="currentColor"
						stroke-width="2"
						stroke-linecap="round"
						stroke-linejoin="round"
					/>
				</svg>
			</button>
		</div>

		<div class="cell status">
			<StatusBadge status={entry.status} />
		</div>

		<div class="cell subject">
			<div class="subject-head">
				<span class="subject-text" title={entry.subject}>{entry.subject}</span>
				{#if live}
					<span class="live-chip"><span class="live-dot"></span>Live</span>
				{/if}
				{#each entry.otherActiveUsers as user (user)}
					<span class="other-call-chip">{user} · on call</span>
				{/each}
			</div>
			<span class="ticket-meta"
				>#{entry.id} · {entry.projectName}{#if startedAt} · {startedAt}{/if}</span
			>
		</div>

		<div class="cell phone col-phone">
			<span class="mono caller">{entry.callerNumber || '—'}</span>
			<span class="mono called">→ {entry.calledNumber ?? '—'}</span>
		</div>

		<div class="cell assignee col-assignee">
			{#if entry.assignee}
				<span>{entry.assignee}</span>
			{:else}
				<span class="muted">unassigned</span>
			{/if}
		</div>

		<div class="cell open">
			<a
				class="op-link"
				href={entry.href}
				target="_blank"
				rel="noopener noreferrer"
				aria-label={`Open ticket “${entry.subject}”`}
				title="Open ticket"
			>
				<svg width="14" height="14" viewBox="0 0 24 24" fill="none" aria-hidden="true">
					<path
						d="M14 4h6v6M20 4l-9 9M18 14v5a1 1 0 0 1-1 1H5a1 1 0 0 1-1-1V7a1 1 0 0 1 1-1h5"
						stroke="currentColor"
						stroke-width="2"
						stroke-linecap="round"
						stroke-linejoin="round"
					/>
				</svg>
			</a>
		</div>
	</div>

	<!-- Narrow-window secondary line: same data as the hidden Caller/Assignee
	     columns, shown only below the breakpoint. -->
	<div class="subline" aria-hidden="true">
		<span class="mono">{entry.callerNumber || '—'} → {entry.calledNumber ?? '—'}</span>
		<span class="dot-sep">·</span>
		{#if entry.assignee}
			<span>{entry.assignee}</span>
		{:else}
			<span class="muted">unassigned</span>
		{/if}
	</div>

	{#if expanded}
		<div class="actions" id={panelId} transition:slide={{ duration: slideDuration }}>
			<CommentThread description={entry.description} />
			<div class="divider" aria-hidden="true"></div>
			<CommentComposer ticketId={entry.id} />
			<div class="divider" aria-hidden="true"></div>
			<CloseButton ticketId={entry.id} status={entry.status} />
		</div>
	{/if}
</div>

<style>
	.ticket {
		position: relative;
		border-bottom: 1px solid var(--color-border);
		background: var(--color-surface);
		animation: enter 220ms ease both;
		animation-delay: var(--enter-delay);
	}
	.ticket.live {
		background: color-mix(in srgb, var(--color-accent) 6%, var(--color-surface));
	}
	.ticket.expanded {
		background: var(--color-surface-2);
	}

	@keyframes enter {
		from {
			opacity: 0;
			transform: translateY(3px);
		}
		to {
			opacity: 1;
			transform: translateY(0);
		}
	}

	.row {
		position: relative;
		display: grid;
		grid-template-columns: var(--ticket-grid);
		align-items: center;
		gap: var(--space-4);
		padding: var(--space-3) var(--space-4);
		cursor: pointer;
	}
	.row:hover {
		background: color-mix(in srgb, var(--color-text) 2%, transparent);
	}

	/* Left rail — neutral and invisible until the row is live. */
	.rail {
		position: absolute;
		left: 0;
		top: 0;
		bottom: 0;
		width: 3px;
		background: transparent;
	}
	.ticket.live .rail {
		background: var(--color-accent);
		animation: pulse 2s ease-in-out infinite;
	}

	@keyframes pulse {
		0%,
		100% {
			opacity: 1;
		}
		50% {
			opacity: 0.4;
		}
	}

	.cell {
		min-width: 0;
	}

	/* — Disclosure — */
	.disclosure-cell {
		display: flex;
		justify-content: flex-start;
	}
	.disclosure {
		display: inline-flex;
		align-items: center;
		justify-content: center;
		width: 1.5rem;
		height: 1.5rem;
		padding: 0;
		border: none;
		border-radius: var(--radius-sm);
		background: transparent;
		color: var(--color-text-muted);
		cursor: pointer;
		transition:
			color 120ms ease,
			background 120ms ease;
	}
	.disclosure:hover {
		color: var(--color-text);
		background: color-mix(in srgb, var(--color-text) 6%, transparent);
	}
	.chevron {
		/* rem so the glyph scales with the viewport-driven root font size,
		   instead of staying pinned to its 14px markup attributes. */
		width: 1rem;
		height: 1rem;
		transition: transform 160ms ease;
	}
	.ticket.expanded .chevron {
		transform: rotate(90deg);
	}

	.subject {
		display: flex;
		flex-direction: column;
		align-items: flex-start;
		justify-content: center;
		gap: 2px;
		min-width: 0;
	}
	.subject-head {
		display: flex;
		align-items: center;
		gap: var(--space-3);
		min-width: 0;
		max-width: 100%;
	}
	.subject-text {
		overflow: hidden;
		text-overflow: ellipsis;
		white-space: nowrap;
		font-weight: 500;
	}
	.ticket-meta {
		max-width: 100%;
		overflow: hidden;
		text-overflow: ellipsis;
		white-space: nowrap;
		font-size: var(--text-xs);
		color: var(--color-text-muted);
	}

	.live-chip {
		flex: none;
		display: inline-flex;
		align-items: center;
		gap: 0.35rem;
		padding: 0.05rem var(--space-2);
		border-radius: var(--radius-sm);
		font-size: var(--text-xs);
		font-weight: 600;
		letter-spacing: 0.04em;
		text-transform: uppercase;
		color: var(--color-accent);
		background: var(--color-accent-tint);
	}
	.live-dot {
		width: 0.4rem;
		height: 0.4rem;
		border-radius: 50%;
		background: var(--color-live-dot);
		animation: pulse 2s ease-in-out infinite;
	}

	/* Another user's live call on this ticket — informational only. Mirrors the
	   live-chip box but stays neutral (no accent, no dot, no pulse) so it never
	   competes with the viewer's own coloured "Live" state. */
	.other-call-chip {
		flex: none;
		display: inline-flex;
		align-items: center;
		padding: 0.05rem var(--space-2);
		border-radius: var(--radius-sm);
		font-size: var(--text-xs);
		font-weight: 500;
		color: var(--color-text-muted);
		background: var(--color-border);
	}

	.phone {
		display: flex;
		flex-direction: column;
		line-height: 1.3;
	}
	.caller {
		font-size: var(--text-sm);
	}
	.called {
		font-size: var(--text-xs);
		color: var(--color-text-muted);
	}

	.assignee {
		font-size: var(--text-base);
		overflow: hidden;
		text-overflow: ellipsis;
		white-space: nowrap;
	}
	.muted {
		color: var(--color-text-faint);
		font-style: italic;
	}

	.open {
		display: flex;
		justify-content: flex-end;
	}
	.op-link {
		display: inline-flex;
		align-items: center;
		justify-content: center;
		width: 1.75rem;
		height: 1.75rem;
		border-radius: var(--radius-sm);
		color: var(--color-text-muted);
		transition:
			color 120ms ease,
			background 120ms ease;
	}
	/* Scale the glyph with the root font size (its 14px markup size no longer
	   tracks the enlarged rows). */
	.op-link svg {
		width: 1.1rem;
		height: 1.1rem;
	}
	.op-link:hover {
		color: var(--color-accent);
		background: var(--color-accent-tint);
	}

	/* — Secondary line (narrow only) — */
	.subline {
		display: none;
		align-items: center;
		gap: var(--space-2);
		padding: 0 var(--space-4) var(--space-3) calc(var(--space-4) + 1.75rem + var(--space-3));
		margin-top: calc(-1 * var(--space-2));
		font-size: var(--text-xs);
		color: var(--color-text-muted);
	}
	.subline .dot-sep {
		color: var(--color-text-faint);
	}

	/* — Expanded actions panel — */
	.actions {
		display: flex;
		flex-direction: column;
		gap: var(--space-4);
		padding: var(--space-4) var(--space-4) var(--space-5);
		/* Indent past the rail + disclosure column so it reads as "belonging" to the row. */
		padding-left: calc(var(--space-4) + 2rem + var(--space-4));
		border-top: 1px solid var(--color-border);
	}
	.divider {
		height: 1px;
		background: var(--color-border);
	}

	/* — Narrow operator window — */
	@media (max-width: 34rem) {
		.col-phone,
		.col-assignee {
			display: none;
		}
		.subline {
			display: flex;
		}
		.actions {
			padding-left: var(--space-4);
		}
	}

	@media (prefers-reduced-motion: reduce) {
		.ticket {
			animation: none;
		}
		.ticket.live .rail,
		.live-dot {
			animation: none;
		}
		.chevron,
		.disclosure,
		.op-link {
			transition: none;
		}
	}
</style>
