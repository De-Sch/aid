<script lang="ts">
	import '../app.css';
	import favicon from '$lib/assets/favicon.svg';
	import { page } from '$app/state';
	import AppHeader from '$lib/components/AppHeader.svelte';
	import Toaster from '$lib/components/Toaster.svelte';

	let { children } = $props();

	// Unauthenticated routes render bare (no app chrome — no header, no
	// "Log out"); everything else gets the frame. /reset is reached via a
	// recovery-key grant with no session, so it belongs here with /login.
	const bare = $derived(page.url.pathname === '/login' || page.url.pathname === '/reset');
</script>

<svelte:head>
	<link rel="icon" href={favicon} />
</svelte:head>

{#if bare}
	{@render children()}
{:else}
	<div class="app">
		<AppHeader />
		<main class="app-main">
			{@render children()}
		</main>
	</div>
{/if}

<!-- Global toast outlet — renders over any route. -->
<Toaster />

<style>
	.app {
		min-height: 100vh;
		display: flex;
		flex-direction: column;
	}

	.app-main {
		flex: 1;
		padding: var(--space-5);
	}
</style>
