<!-- Close one ticket via POST /ui/close/{id} (no body; the daemon runs the
     two-step status flow). Requires an explicit confirm step. Like the comment
     composer, it never patches the ticket list — success is reconciled by the
     store's WS action_result toast plus a single-row ticket_remove that drops
     just this ticket in place (status leaves the board) — no whole-board
     refetch. Inline error surfaces a business failure (200 ok:false) or a
     thrown ApiError. -->
<script lang="ts">
	import { goto } from '$app/navigation';
	import { client, AuthError, describeError } from '$lib/api/client.js';
	import type { TicketStatus } from '$lib/api/types.js';

	let { ticketId, status }: { ticketId: string; status: TicketStatus } = $props();

	let confirming = $state(false);
	let submitting = $state(false);
	let errorMsg = $state<string | null>(null);

	// Already terminal — nothing to close.
	const alreadyClosed = $derived(status === 'Closed');

	async function close() {
		submitting = true;
		errorMsg = null;
		try {
			const res = await client.closeTicket(ticketId);
			if (res?.ok) {
				confirming = false;
			} else {
				errorMsg = res?.message ?? 'Could not close the ticket.';
				confirming = false;
			}
		} catch (e) {
			if (e instanceof AuthError) {
				void goto('/login');
				return;
			}
			errorMsg = describeError(e);
			confirming = false;
		} finally {
			submitting = false;
		}
	}
</script>

<div class="close">
	{#if alreadyClosed}
		<span class="muted">This ticket is already closed.</span>
	{:else if !confirming}
		<button
			class="trigger"
			type="button"
			onclick={() => {
				errorMsg = null;
				confirming = true;
			}}
		>
			Close ticket
		</button>
	{:else}
		<div class="confirm" role="group" aria-label="Confirm closing the ticket">
			<span class="confirm-q">Close this ticket?</span>
			<button class="confirm-yes" type="button" disabled={submitting} onclick={close}>
				{submitting ? 'Closing…' : 'Confirm'}
			</button>
			<button
				class="confirm-no"
				type="button"
				disabled={submitting}
				onclick={() => (confirming = false)}
			>
				Cancel
			</button>
		</div>
	{/if}

	{#if errorMsg}
		<p class="error" role="alert">{errorMsg}</p>
	{/if}
</div>

<style>
	.close {
		display: flex;
		flex-direction: column;
		gap: var(--space-2);
		align-items: flex-start;
	}

	.muted {
		font-size: 0.85rem;
		font-style: italic;
		color: var(--color-text-muted);
	}

	/* Idle trigger — outlined rose, the heavier action. */
	.trigger {
		padding: var(--space-2) var(--space-4);
		border: 1px solid color-mix(in srgb, var(--color-danger) 45%, var(--color-border));
		border-radius: var(--radius-sm);
		background: transparent;
		color: var(--color-danger);
		font-weight: 600;
		font-size: 0.85rem;
		cursor: pointer;
		transition:
			background 120ms ease,
			border-color 120ms ease;
	}
	.trigger:hover {
		background: var(--st-rejected-bg);
		border-color: var(--color-danger);
	}
	.trigger:focus-visible {
		outline: 2px solid var(--color-danger);
		outline-offset: 2px;
	}

	.confirm {
		display: flex;
		align-items: center;
		gap: var(--space-3);
		flex-wrap: wrap;
	}
	.confirm-q {
		font-size: 0.85rem;
		font-weight: 600;
		color: var(--color-text);
	}

	.confirm-yes {
		padding: var(--space-2) var(--space-4);
		border: none;
		border-radius: var(--radius-sm);
		background: var(--color-danger-solid);
		color: var(--color-on-danger);
		font-weight: 600;
		font-size: 0.85rem;
		cursor: pointer;
		transition: filter 120ms ease;
	}
	.confirm-yes:hover:not(:disabled) {
		filter: brightness(1.08);
	}
	.confirm-yes:focus-visible {
		outline: 2px solid var(--color-danger);
		outline-offset: 2px;
	}
	.confirm-yes:disabled {
		opacity: 0.5;
		cursor: default;
	}

	.confirm-no {
		padding: var(--space-2) var(--space-3);
		border: 1px solid var(--color-border);
		border-radius: var(--radius-sm);
		background: var(--color-surface);
		color: var(--color-text-muted);
		font-size: 0.85rem;
		cursor: pointer;
		transition:
			color 120ms ease,
			background 120ms ease;
	}
	.confirm-no:hover:not(:disabled) {
		color: var(--color-text);
		background: var(--color-bg);
	}
	.confirm-no:focus-visible {
		outline: 2px solid var(--color-accent);
		outline-offset: 1px;
	}
	.confirm-no:disabled {
		opacity: 0.5;
		cursor: default;
	}

	.error {
		margin: 0;
		font-size: 0.85rem;
		color: var(--st-rejected-fg);
	}

	@media (prefers-reduced-motion: reduce) {
		.trigger,
		.confirm-yes,
		.confirm-no {
			transition: none;
		}
	}
</style>
