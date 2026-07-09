<script lang="ts">
	import { goto } from '$app/navigation';
	import { session } from '$lib/stores/session.svelte';
	import { AuthError, RateLimitError, ProtocolError } from '$lib/api/client.js';

	const MIN_LEN = 1;

	let password = $state('');
	let confirm = $state('');
	let message = $state<string | null>(null);
	let submitting = $state(false);

	async function handleSubmit(e: SubmitEvent) {
		e.preventDefault();
		if (submitting) return;
		message = null;

		// Validate locally for a fast, friendly message; the daemon enforces
		// the same rules server-side regardless.
		if (password.length < MIN_LEN) {
			message = 'Password is required';
			return;
		}
		if (password !== confirm) {
			message = 'Passwords do not match';
			return;
		}

		submitting = true;
		try {
			await session.resetPassword(password);
			// No session is created by a reset — send the user to sign in fresh.
			await goto('/login');
		} catch (err) {
			if (err instanceof AuthError) {
				message = 'This reset link has expired. Please start again from the login page.';
			} else if (err instanceof RateLimitError) {
				message = 'Too many attempts, please try again in a moment';
			} else if (err instanceof ProtocolError) {
				message = 'Password is required';
			} else {
				message = 'Something went wrong. Please try again.';
			}
		} finally {
			submitting = false;
		}
	}
</script>

<div class="reset-page">
	<div class="atmosphere" aria-hidden="true"></div>
	<form class="card" onsubmit={handleSubmit}>
		<span class="brand-mark" aria-hidden="true"></span>
		<h1 class="title">Set a new password</h1>
		<p class="subtitle">Choose a new password for your account</p>

		<label class="field">
			<span class="field-label">New password</span>
			<input
				name="new-password"
				type="password"
				autocomplete="new-password"
				bind:value={password}
				disabled={submitting}
			/>
		</label>

		<label class="field">
			<span class="field-label">Confirm password</span>
			<input
				name="confirm-password"
				type="password"
				autocomplete="new-password"
				bind:value={confirm}
				disabled={submitting}
			/>
		</label>

		{#if message}
			<p class="error" role="alert">{message}</p>
		{/if}

		<button class="submit" type="submit" disabled={submitting}>
			{submitting ? 'Saving…' : 'Save new password'}
		</button>
	</form>
</div>

<style>
	.reset-page {
		position: relative;
		min-height: 100vh;
		display: flex;
		align-items: center;
		justify-content: center;
		padding: var(--space-5);
		overflow: hidden;
	}

	/* Faint atmosphere — two low-intensity teal washes. Matches the login page. */
	.atmosphere {
		position: absolute;
		inset: 0;
		z-index: 0;
		background:
			radial-gradient(
				40rem 30rem at 18% 0%,
				color-mix(in srgb, var(--color-accent) 9%, transparent),
				transparent 70%
			),
			radial-gradient(
				36rem 28rem at 100% 100%,
				color-mix(in srgb, var(--color-accent) 7%, transparent),
				transparent 70%
			);
	}

	.card {
		position: relative;
		z-index: 1;
		width: 100%;
		max-width: 22rem;
		display: flex;
		flex-direction: column;
		gap: var(--space-4);
		padding: var(--space-6);
		background: var(--color-surface);
		border: 1px solid var(--color-border);
		border-radius: var(--radius-lg);
		box-shadow: var(--shadow-md);
		animation: rise 280ms ease both;
	}

	@keyframes rise {
		from {
			opacity: 0;
			transform: translateY(8px);
		}
		to {
			opacity: 1;
			transform: translateY(0);
		}
	}

	.brand-mark {
		width: 0.7rem;
		height: 0.7rem;
		border-radius: 3px;
		background: var(--color-accent);
		transform: rotate(45deg);
	}

	.title {
		font-size: var(--text-lg);
		font-weight: 700;
		letter-spacing: var(--track-tight);
	}

	.subtitle {
		margin-top: calc(-1 * var(--space-3));
		color: var(--color-text-muted);
		font-size: var(--text-base);
	}

	.field {
		display: flex;
		flex-direction: column;
		gap: var(--space-1);
	}

	.field-label {
		font-size: var(--text-sm);
		color: var(--color-text-muted);
	}

	.field input {
		padding: var(--space-2) var(--space-3);
		border: 1px solid var(--color-border);
		border-radius: var(--radius-sm);
		background: var(--color-surface-2);
		transition: border-color 120ms ease;
	}
	.field input:hover {
		border-color: var(--color-border-strong);
	}

	.error {
		color: var(--color-danger);
		font-size: var(--text-base);
	}

	.submit {
		padding: var(--space-3);
		border: none;
		border-radius: var(--radius-sm);
		background: var(--color-accent);
		color: var(--color-accent-contrast);
		font-weight: 600;
		cursor: pointer;
		transition: background 120ms ease;
	}
	.submit:hover:not(:disabled) {
		background: var(--color-accent-hover);
	}

	.submit:disabled {
		opacity: 0.6;
		cursor: default;
	}

	@media (prefers-reduced-motion: reduce) {
		.card {
			animation: none;
		}
		.field input,
		.submit {
			transition: none;
		}
	}
</style>
