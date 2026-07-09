<!-- Read-only view of a ticket's comment/history section. The backend stores
     everything in the ticket's `description` field: auto-generated call-log
     lines (e.g. "alice: Call start: … (callid)") interleaved with the freeform
     notes posted via the comment composer. We render it verbatim, one line per
     entry, with the machine call-log lines lightly de-emphasised so the typed
     notes stand out. This is exactly where a "Send comment" lands, so after a
     comment is posted the store's single-row ticket_upsert merges this ticket's
     new `description` in place (no whole-board refetch).
     Presentation only — no coupling to the log format beyond a cosmetic hint. -->
<script lang="ts">
	let { description = '' }: { description?: string } = $props();

	// Split into trimmed, non-empty lines. Order is chronological (oldest first),
	// matching how the description is built and how OpenProject shows it.
	const lines = $derived(
		description
			.split('\n')
			.map((l) => l.trim())
			.filter((l) => l.length > 0)
	);

	// Cosmetic only: the auto-generated call lines carry these markers.
	const isCallLog = (line: string) => line.includes('Call start:') || line.includes('Call End:');
</script>

<section class="thread" aria-label="Ticket comments">
	<span class="label">Comments</span>

	{#if lines.length === 0}
		<p class="empty">No comments yet.</p>
	{:else}
		<ol class="entries">
			{#each lines as line, i (i)}
				<li class="entry" class:system={isCallLog(line)}>{line}</li>
			{/each}
		</ol>
	{/if}
</section>

<style>
	.thread {
		display: flex;
		flex-direction: column;
		gap: var(--space-2);
	}

	.label {
		font-size: 0.75rem;
		font-weight: 600;
		letter-spacing: 0.04em;
		text-transform: uppercase;
		color: var(--color-text-muted);
	}

	.empty {
		margin: 0;
		font-size: 0.85rem;
		font-style: italic;
		color: var(--color-text-faint);
	}

	.entries {
		display: flex;
		flex-direction: column;
		gap: var(--space-1);
		margin: 0;
		padding: var(--space-2) var(--space-3);
		list-style: none;
		border: 1px solid var(--color-border);
		border-radius: var(--radius-sm);
		background: var(--color-surface);
		/* Keep a long history from pushing the composer off-screen. */
		max-height: 14rem;
		overflow-y: auto;
	}

	.entry {
		font-size: 0.9rem;
		line-height: 1.45;
		color: var(--color-text);
		/* Preserve any internal spacing without letting a long line overflow. */
		white-space: pre-wrap;
		overflow-wrap: anywhere;
	}

	/* Auto-generated call-log lines: quieter and monospaced so typed notes lead. */
	.entry.system {
		font-family: var(--font-mono, ui-monospace, monospace);
		font-size: 0.8rem;
		color: var(--color-text-muted);
	}
</style>
