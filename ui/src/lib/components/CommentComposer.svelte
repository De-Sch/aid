<!-- Append a comment to one ticket. Posts /ui/comment/{id} and returns the
     ActionResult, but DELIBERATELY does not patch the ticket list: success is
     reconciled by the store's WS action_result toast plus a single-row
     ticket_upsert that merges just this ticket in place — no whole-board
     refetch (dashboard.svelte.ts). This component only owns its own in-flight + failure
     state. A business failure (200 ok:false) is surfaced inline using the
     server's `message`; a thrown ApiError (4xx/5xx, no WS frame) is surfaced
     inline too. -->
<script lang="ts">
	import { goto } from '$app/navigation';
	import { client, AuthError, describeError } from '$lib/api/client.js';

	let { ticketId }: { ticketId: string } = $props();

	let text = $state('');
	let submitting = $state(false);
	let errorMsg = $state<string | null>(null);

	// Empty / whitespace-only submissions are blocked.
	const canSubmit = $derived(text.trim().length > 0 && !submitting);

	async function submit() {
		if (!canSubmit) return;
		submitting = true;
		errorMsg = null;
		try {
			const res = await client.postComment(ticketId, text.trim());
			if (res?.ok) {
				// Success: clear the field and let the toast + refetch take over.
				text = '';
			} else {
				errorMsg = res?.message ?? 'Could not save the comment.';
			}
		} catch (e) {
			if (e instanceof AuthError) {
				void goto('/login');
				return;
			}
			errorMsg = describeError(e);
		} finally {
			submitting = false;
		}
	}

	function onsubmit(e: SubmitEvent) {
		e.preventDefault();
		void submit();
	}

	// Cmd/Ctrl+Enter submits from inside the textarea.
	function onkeydown(e: KeyboardEvent) {
		if ((e.metaKey || e.ctrlKey) && e.key === 'Enter') {
			e.preventDefault();
			void submit();
		}
	}
</script>

<form class="composer" {onsubmit}>
	<label class="field">
		<span class="label">Add a comment</span>
		<textarea
			class="input"
			rows="2"
			placeholder="Write a note for this ticket…"
			bind:value={text}
			disabled={submitting}
			{onkeydown}
		></textarea>
	</label>

	{#if errorMsg}
		<p class="error" role="alert">{errorMsg}</p>
	{/if}

	<div class="actions-bar">
		<span class="hint" class:visible={submitting}>Sending…</span>
		<button class="send" type="submit" disabled={!canSubmit}>
			{submitting ? 'Sending…' : 'Send comment'}
		</button>
	</div>
</form>

<style>
	.composer {
		display: flex;
		flex-direction: column;
		gap: var(--space-2);
	}

	.field {
		display: flex;
		flex-direction: column;
		gap: var(--space-1);
	}
	.label {
		font-size: 0.75rem;
		font-weight: 600;
		letter-spacing: 0.04em;
		text-transform: uppercase;
		color: var(--color-text-muted);
	}

	.input {
		width: 100%;
		padding: var(--space-2) var(--space-3);
		border: 1px solid var(--color-border);
		border-radius: var(--radius-sm);
		background: var(--color-surface);
		color: var(--color-text);
		font: inherit;
		line-height: 1.4;
		resize: vertical;
		min-height: 2.75rem;
		transition: border-color 120ms ease;
	}
	.input::placeholder {
		color: var(--color-text-muted);
	}
	.input:focus {
		outline: 2px solid var(--color-accent);
		outline-offset: 1px;
		border-color: transparent;
	}
	.input:disabled {
		opacity: 0.6;
	}

	.error {
		margin: 0;
		font-size: 0.85rem;
		color: var(--st-rejected-fg);
	}

	.actions-bar {
		display: flex;
		align-items: center;
		justify-content: flex-end;
		gap: var(--space-3);
	}

	.hint {
		font-size: 0.8rem;
		color: var(--color-text-muted);
		opacity: 0;
		transition: opacity 120ms ease;
	}
	.hint.visible {
		opacity: 1;
	}

	.send {
		padding: var(--space-2) var(--space-4);
		border: none;
		border-radius: var(--radius-sm);
		background: var(--color-accent);
		color: var(--color-accent-contrast);
		font-weight: 600;
		font-size: 0.85rem;
		cursor: pointer;
		transition:
			opacity 120ms ease,
			filter 120ms ease;
	}
	.send:hover:not(:disabled) {
		filter: brightness(1.06);
	}
	.send:focus-visible {
		outline: 2px solid var(--color-accent);
		outline-offset: 2px;
	}
	.send:disabled {
		opacity: 0.5;
		cursor: default;
	}

	@media (prefers-reduced-motion: reduce) {
		.input,
		.hint,
		.send {
			transition: none;
		}
	}
</style>
