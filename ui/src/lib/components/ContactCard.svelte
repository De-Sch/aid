<!-- The CardDAV match for the active caller (addressCallInformation).
     Renders NOTHING when contact is null — no empty shell — so the spotlight
     can drop it in unconditionally and it simply vanishes on a no-match. -->
<script lang="ts">
	import type { Contact } from '$lib/api/types.js';

	let { contact }: { contact: Contact | null } = $props();
</script>

{#if contact}
	<aside class="contact" aria-label="Caller contact">
		<div class="head">
			<div class="identity">
				<span class="name">{contact.name}</span>
				{#if contact.companyName}
					<span class="company">{contact.companyName}</span>
				{/if}
			</div>
			<span class="kind" data-kind={contact.kind}>{contact.kind}</span>
		</div>

		{#if contact.phoneNumbers.length}
			<div class="field">
				<span class="field-label">Phone</span>
				<ul class="phones">
					{#each contact.phoneNumbers as number (number)}
						<li class="mono">{number}</li>
					{/each}
				</ul>
			</div>
		{/if}

		{#if contact.projectIds.length}
			<div class="field">
				<span class="field-label">Projects</span>
				<div class="tags">
					{#each contact.projectIds as id (id)}
						<span class="tag">{id}</span>
					{/each}
				</div>
			</div>
		{/if}
	</aside>
{/if}

<style>
	.contact {
		display: flex;
		flex-direction: column;
		gap: var(--space-3);
		padding: var(--space-4);
		background: var(--color-surface);
		border: 1px solid var(--color-border);
		border-radius: var(--radius-md);
	}

	.head {
		display: flex;
		align-items: flex-start;
		justify-content: space-between;
		gap: var(--space-3);
	}

	.identity {
		display: flex;
		flex-direction: column;
		min-width: 0;
	}
	.name {
		font-weight: 600;
		line-height: 1.25;
		overflow: hidden;
		text-overflow: ellipsis;
		white-space: nowrap;
	}
	.company {
		font-size: 0.85rem;
		color: var(--color-text-muted);
		overflow: hidden;
		text-overflow: ellipsis;
		white-space: nowrap;
	}

	/* Person vs Company — a quiet outline pill, not another loud signal. */
	.kind {
		flex: none;
		padding: 0.1rem var(--space-2);
		border-radius: var(--radius-sm);
		font-size: 0.66rem;
		font-weight: 600;
		letter-spacing: 0.05em;
		text-transform: uppercase;
		color: var(--st-new-fg);
		background: var(--st-new-bg);
	}
	.kind[data-kind='Company'] {
		color: var(--st-tested-fg);
		background: var(--st-tested-bg);
	}

	.field {
		display: flex;
		flex-direction: column;
		gap: var(--space-1);
	}
	.field-label {
		font-size: 0.64rem;
		font-weight: 600;
		letter-spacing: 0.07em;
		text-transform: uppercase;
		color: var(--color-text-muted);
	}

	.phones {
		display: flex;
		flex-direction: column;
		gap: 0.1rem;
		margin: 0;
		padding: 0;
		list-style: none;
	}
	.phones li {
		font-size: 0.88rem;
	}

	.tags {
		display: flex;
		flex-wrap: wrap;
		gap: var(--space-1);
	}
	.tag {
		padding: 0.05rem var(--space-2);
		border: 1px solid var(--color-border);
		border-radius: var(--radius-sm);
		font-size: 0.74rem;
		color: var(--color-text-muted);
		background: var(--color-bg);
	}
</style>
