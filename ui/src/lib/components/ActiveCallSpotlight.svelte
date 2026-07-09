<!-- The focal panel: what the agent stares at while the phone rings. Shown only
     when dashboard.active != null. Carries the same teal live-highlight as
     the owning ticket row (TicketRow .live) so the spotlight and its row read as
     one thing. The resolved caller Contact drops in via ContactCard, which
     renders nothing on a no-match — so the panel never shows an empty shell. -->
<script lang="ts">
	import { dashboard } from '$lib/stores/dashboard.svelte';
	import ContactCard from './ContactCard.svelte';

	const active = $derived(dashboard.active);
	const contact = $derived(dashboard.addressCallInformation);

	// The active call summary carries no href; resolve the owning ticket from the
	// list to build the "open ticket" link. May be null mid-refresh.
	const owner = $derived(
		active ? (dashboard.tickets.find((t) => t.id === active.ticketId) ?? null) : null
	);
</script>

{#if active}
	<section class="spotlight" aria-label="Active call">
		<div class="rail" aria-hidden="true"></div>

		<div class="call">
			<span class="eyebrow">
				<span class="ring" aria-hidden="true"></span>
				Active call
			</span>

			<span class="mono number">{active.callerNumber}</span>

			<div class="meta">
				{#if active.projectName}
					<span class="project">{active.projectName}</span>
				{/if}
				{#if owner}
					<a
						class="ticket-link"
						href={owner.href}
						target="_blank"
						rel="noopener noreferrer"
						title="Open ticket"
					>
						<span>Ticket #{active.ticketId}</span>
						<svg width="13" height="13" viewBox="0 0 24 24" fill="none" aria-hidden="true">
							<path
								d="M14 4h6v6M20 4l-9 9M18 14v5a1 1 0 0 1-1 1H5a1 1 0 0 1-1-1V7a1 1 0 0 1 1-1h5"
								stroke="currentColor"
								stroke-width="2"
								stroke-linecap="round"
								stroke-linejoin="round"
							/>
						</svg>
					</a>
				{:else}
					<span class="ticket-ref mono">#{active.ticketId}</span>
				{/if}
			</div>
		</div>

		{#if contact}
			<ContactCard {contact} />
		{:else}
			<p class="no-match">No matching contact</p>
		{/if}
	</section>
{/if}

<style>
	.spotlight {
		position: relative;
		display: grid;
		grid-template-columns: minmax(0, 1fr) minmax(0, 18rem);
		gap: var(--space-5);
		align-items: start;
		padding: var(--space-5);
		padding-left: calc(var(--space-5) + 3px);
		border: 1px solid color-mix(in srgb, var(--color-accent) 35%, var(--color-border));
		border-radius: var(--radius-lg);
		/* Accent-tinted surface with a soft glow rising from the live rail. */
		background:
			radial-gradient(
				120% 80% at 0% 0%,
				color-mix(in srgb, var(--color-accent) 12%, transparent),
				transparent 60%
			),
			color-mix(in srgb, var(--color-accent) 7%, var(--color-surface));
		box-shadow: var(--shadow-sm);
		overflow: hidden;
	}

	/* Same teal rail + slow pulse the owning TicketRow.live carries. */
	.rail {
		position: absolute;
		left: 0;
		top: 0;
		bottom: 0;
		width: 3px;
		background: var(--color-accent);
		animation: pulse 2s ease-in-out infinite;
	}

	.call {
		display: flex;
		flex-direction: column;
		gap: var(--space-2);
		min-width: 0;
	}

	.eyebrow {
		display: inline-flex;
		align-items: center;
		gap: var(--space-2);
		font-size: 0.7rem;
		font-weight: 600;
		letter-spacing: 0.08em;
		text-transform: uppercase;
		color: var(--color-accent);
	}

	/* A soft expanding ring — the "phone is live" pulse. */
	.ring {
		position: relative;
		width: 0.5rem;
		height: 0.5rem;
		border-radius: 50%;
		background: var(--color-live-dot);
	}
	.ring::after {
		content: '';
		position: absolute;
		inset: 0;
		border-radius: 50%;
		background: var(--color-live-dot);
		animation: ring 2s ease-out infinite;
	}

	.number {
		font-size: var(--text-xl);
		font-weight: 500;
		letter-spacing: 0.01em;
		line-height: 1.1;
		color: var(--color-text);
	}

	.meta {
		display: flex;
		flex-wrap: wrap;
		align-items: center;
		gap: var(--space-2) var(--space-3);
		margin-top: var(--space-1);
	}

	.project {
		padding: 0.1rem var(--space-2);
		border-radius: var(--radius-sm);
		font-size: var(--text-sm);
		color: var(--color-accent);
		background: var(--color-accent-tint);
	}

	.ticket-link {
		display: inline-flex;
		align-items: center;
		gap: 0.35rem;
		font-size: 0.85rem;
		font-weight: 500;
		text-decoration: none;
		color: var(--color-accent);
		transition: opacity 120ms ease;
	}
	/* em → the icon tracks the link's font-size (rem-based), so it scales with
	   the root font instead of its fixed 13px markup size. */
	.ticket-link svg {
		width: 1em;
		height: 1em;
	}
	.ticket-link:hover {
		opacity: 0.75;
	}
	.ticket-link:focus-visible {
		outline: 2px solid var(--color-accent);
		outline-offset: 2px;
		border-radius: var(--radius-sm);
	}

	.ticket-ref {
		font-size: 0.85rem;
		color: var(--color-text-muted);
	}

	.no-match {
		margin: 0;
		align-self: center;
		font-size: var(--text-sm);
		font-style: italic;
		color: var(--color-text-faint);
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
	@keyframes ring {
		0% {
			transform: scale(1);
			opacity: 0.5;
		}
		100% {
			transform: scale(2.6);
			opacity: 0;
		}
	}

	/* Stack the contact under the call on narrow viewports. */
	@media (max-width: 40rem) {
		.spotlight {
			grid-template-columns: minmax(0, 1fr);
			gap: var(--space-4);
		}
	}

	@media (prefers-reduced-motion: reduce) {
		.rail,
		.ring::after {
			animation: none;
		}
	}
</style>
