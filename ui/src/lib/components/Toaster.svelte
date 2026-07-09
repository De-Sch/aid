<!-- Fixed bottom-right outlet for the toast channel. Subscribes to
     toasts.items and animates each in/out. Mounted once in +layout.svelte. -->
<script lang="ts">
	import { fly } from 'svelte/transition';
	import { toasts } from '$lib/stores/toasts.svelte';
	import Toast from './Toast.svelte';
</script>

<div class="toaster" aria-live="polite" aria-relevant="additions">
	{#each toasts.items as toast (toast.id)}
		<div transition:fly={{ y: 12, duration: 200 }}>
			<Toast {toast} />
		</div>
	{/each}
</div>

<style>
	.toaster {
		position: fixed;
		right: var(--space-5);
		bottom: var(--space-5);
		z-index: 50;
		display: flex;
		flex-direction: column;
		gap: var(--space-2);
		/* Let clicks fall through the empty area; toasts re-enable pointer events. */
		pointer-events: none;
	}
	.toaster > :global(*) {
		pointer-events: auto;
	}
</style>
