<!-- App frame header: mark · spacer · whoami · theme toggle · health pill · log out.
     Owns the logout action (moved out of +layout.svelte). -->
<script lang="ts">
	import { goto } from '$app/navigation';
	import { session } from '$lib/stores/session.svelte';
	import HealthPill from './HealthPill.svelte';
	import ThemeToggle from './ThemeToggle.svelte';

	async function handleLogout() {
		await session.logout();
		await goto('/login');
	}
</script>

<header class="app-header">
	<span class="brand">
		<span class="brand-mark" aria-hidden="true"></span>
		<span class="brand-name">AID<span class="brand-sub">Dashboard</span></span>
	</span>
	<div class="spacer"></div>
	{#if session.username}
		<span class="user mono">{session.username}</span>
	{/if}
	<ThemeToggle />
	<HealthPill />
	<button type="button" class="logout" onclick={handleLogout}>Log out</button>
</header>

<style>
	.app-header {
		display: flex;
		align-items: center;
		gap: var(--space-3);
		padding: var(--space-3) var(--space-5);
		background: var(--color-surface);
		border-bottom: 1px solid var(--color-border);
	}

	.brand {
		display: inline-flex;
		align-items: center;
		gap: var(--space-2);
	}
	.brand-mark {
		width: 0.55rem;
		height: 0.55rem;
		border-radius: 2px;
		background: var(--color-accent);
		transform: rotate(45deg);
	}
	.brand-name {
		font-weight: 700;
		letter-spacing: var(--track-tight);
	}
	.brand-sub {
		margin-left: 0.4em;
		font-weight: 500;
		color: var(--color-text-muted);
	}

	.spacer {
		flex: 1;
	}

	.user {
		color: var(--color-text-muted);
		font-size: var(--text-base);
	}

	.logout {
		padding: var(--space-1) var(--space-3);
		border: 1px solid var(--color-border);
		border-radius: var(--radius-sm);
		background: var(--color-surface);
		color: var(--color-text-muted);
		cursor: pointer;
		transition:
			color 120ms ease,
			border-color 120ms ease;
	}
	.logout:hover {
		color: var(--color-text);
		border-color: var(--color-border-strong);
	}

	@media (prefers-reduced-motion: reduce) {
		.logout {
			transition: none;
		}
	}

	@media (max-width: 30rem) {
		.brand-sub,
		.user {
			display: none;
		}
		.app-header {
			gap: var(--space-2);
			padding: var(--space-3) var(--space-4);
		}
	}
</style>
