<!-- Header control: cycles theme auto → light → dark → auto. Shows the icon of
     the active preference; the label announces it for screen readers. -->
<script lang="ts">
	import { theme } from '$lib/stores/theme.svelte';

	const labels = {
		auto: 'Theme: follow system. Activate to switch to light.',
		light: 'Theme: light. Activate to switch to dark.',
		dark: 'Theme: dark. Activate to switch to system.'
	} as const;
</script>

<button
	type="button"
	class="toggle"
	onclick={() => theme.cycle()}
	aria-label={labels[theme.mode]}
	title={labels[theme.mode]}
>
	{#if theme.mode === 'light'}
		<!-- sun -->
		<svg width="16" height="16" viewBox="0 0 24 24" fill="none" aria-hidden="true">
			<circle cx="12" cy="12" r="4.2" stroke="currentColor" stroke-width="1.8" />
			<path
				d="M12 2.5v2.2M12 19.3v2.2M21.5 12h-2.2M4.7 12H2.5M18.7 5.3l-1.6 1.6M6.9 17.1l-1.6 1.6M18.7 18.7l-1.6-1.6M6.9 6.9 5.3 5.3"
				stroke="currentColor"
				stroke-width="1.8"
				stroke-linecap="round"
			/>
		</svg>
	{:else if theme.mode === 'dark'}
		<!-- moon -->
		<svg width="16" height="16" viewBox="0 0 24 24" fill="none" aria-hidden="true">
			<path
				d="M20 14.2A8 8 0 1 1 9.8 4a6.4 6.4 0 0 0 10.2 10.2Z"
				stroke="currentColor"
				stroke-width="1.8"
				stroke-linejoin="round"
			/>
		</svg>
	{:else}
		<!-- auto: half-filled disc -->
		<svg width="16" height="16" viewBox="0 0 24 24" fill="none" aria-hidden="true">
			<circle cx="12" cy="12" r="8.2" stroke="currentColor" stroke-width="1.8" />
			<path d="M12 3.8a8.2 8.2 0 0 0 0 16.4Z" fill="currentColor" />
		</svg>
	{/if}
</button>

<style>
	.toggle {
		display: inline-flex;
		align-items: center;
		justify-content: center;
		width: 1.85rem;
		height: 1.85rem;
		padding: 0;
		border: 1px solid var(--color-border);
		border-radius: var(--radius-sm);
		background: var(--color-surface);
		color: var(--color-text-muted);
		cursor: pointer;
		transition:
			color 120ms ease,
			border-color 120ms ease,
			background 120ms ease;
	}
	.toggle:hover {
		color: var(--color-text);
		border-color: var(--color-border-strong);
	}

	@media (prefers-reduced-motion: reduce) {
		.toggle {
			transition: none;
		}
	}
</style>
