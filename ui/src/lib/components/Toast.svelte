<!-- A single toast. Auto-dismiss is scheduled by the store, so this component
     only renders + offers a manual dismiss. Kind maps to a calm accent. -->
<script lang="ts">
	import { toasts, type Toast } from '$lib/stores/toasts.svelte';

	let { toast }: { toast: Toast } = $props();
</script>

<div class="toast" data-kind={toast.kind} role="status">
	<span class="accent" aria-hidden="true"></span>
	<span class="message">{toast.message}</span>
	<button
		type="button"
		class="dismiss"
		aria-label="Dismiss notification"
		onclick={() => toasts.dismiss(toast.id)}
	>
		<svg width="12" height="12" viewBox="0 0 24 24" fill="none" aria-hidden="true">
			<path
				d="M6 6l12 12M18 6L6 18"
				stroke="currentColor"
				stroke-width="2.2"
				stroke-linecap="round"
			/>
		</svg>
	</button>
</div>

<style>
	.toast {
		position: relative;
		display: flex;
		align-items: center;
		gap: var(--space-3);
		min-width: 16rem;
		max-width: 24rem;
		padding: var(--space-3) var(--space-3) var(--space-3) var(--space-4);
		background: var(--color-surface);
		border: 1px solid var(--color-border);
		border-radius: var(--radius-md);
		box-shadow: var(--shadow-md);
		overflow: hidden;
	}

	.accent {
		position: absolute;
		left: 0;
		top: 0;
		bottom: 0;
		width: 3px;
		background: var(--color-info);
	}
	.toast[data-kind='success'] .accent {
		background: var(--color-success);
	}
	.toast[data-kind='error'] .accent {
		background: var(--color-danger);
	}

	.message {
		flex: 1;
		font-size: 0.88rem;
		line-height: 1.35;
	}

	.dismiss {
		flex: none;
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
		transition: background 120ms ease;
	}
	.dismiss:hover {
		background: var(--color-bg);
		color: var(--color-text);
	}
	.dismiss:focus-visible {
		outline: 2px solid var(--color-accent);
		outline-offset: 1px;
	}
</style>
