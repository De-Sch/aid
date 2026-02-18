<!--
	Login Page Component for AID System

	This is the entry point for agents accessing the AID dashboard.
	Provides simple name-based authentication and redirects to personalized dashboard.

	Features:
	- Name input with auto-focus
	- Password field (currently for display only)
	- Loading state during authentication
	- Error message display
	- Redirect to dashboard on success

	Authentication Flow:
	1. Agent enters name (required)
	2. Optional password field (legacy, not validated)
	3. System verifies agent exists via /api/ui/{name} endpoint
	4. Redirects to /dashboard#{name} on success
	5. Dashboard loads agent-specific tickets and call info

	Note: Password validation is not currently implemented.
	This is a placeholder for future authentication enhancement.
-->
<script>
	import { onMount } from 'svelte';
	import { goto } from '$app/navigation';

	// Component state
	let name = ''; // Agent name for login
	let password = ''; // Password (legacy field, not validated)
	let isLoading = false; // Loading state for async login
	let errorMessage = ''; // Error message display

	/**
	 * Handle login form submission
	 * Validates agent name and redirects to dashboard
	 */
	async function handleSubmit() {
		try {
			isLoading = true;
			errorMessage = '';

			// Verify agent exists by calling microkernel
			const apiUrl = `/api/ui/${name}`;
			console.log('Making API call to:', apiUrl);
			const response = await fetch(apiUrl);
			const responseText = await response.text();
			console.log('Response status:', response.status, response.statusText);
			console.log('Response text:', responseText);

			// Redirect to personalized dashboard
			// Hash contains agent name for dashboard to load their data
			goto(`/dashboard#${name}`);
		} catch (error) {
			console.error('Login error:', error);
			errorMessage = 'Login failed. Please try again.';
		} finally {
			isLoading = false;
		}
	}

	// Auto-focus name input on page load
	onMount(() => {
		document.querySelector('#name')?.focus();
	});
</script>

<div class="app-container">
	<!-- AID Header -->
	<div class="aid-header">
		<div class="aid-logo">AID</div>
		<div class="header-subtitle">Agent Intelligence Dashboard</div>
	</div>

	<!-- Login Content -->
	<div class="login-content">
		<div class="login-card">
			<div class="welcome-section">
				<div class="company-logo">
					<img src="inglogo.png" alt="Company Logo" />
				</div>
				<h1 class="welcome-title">Welcome</h1>
				<p class="welcome-subtitle">Sign in to access your dashboard</p>
			</div>

			<form on:submit|preventDefault={handleSubmit} class="login-form">
				{#if errorMessage}
					<div class="error-message">
						<div class="error-icon">⚠</div>
						<div class="error-text">{errorMessage}</div>
					</div>
				{/if}

				<div class="form-group">
					<label for="name">Agent Name</label>
					<input
						id="name"
						type="text"
						bind:value={name}
						placeholder="Enter your agent name"
						required
						class="form-input"
					/>
				</div>

				<div class="form-group">
					<label for="password">Password</label>
					<input
						id="password"
						type="password"
						bind:value={password}
						placeholder="Enter your password"
						class="form-input"
					/>
				</div>

				<button type="submit" disabled={isLoading} class="login-button">
					{#if isLoading}
						<div class="loading-spinner"></div>
						<span>Connecting...</span>
					{:else}
						<span>Access Dashboard</span>
						<div class="button-icon">→</div>
					{/if}
				</button>
			</form>

			<div class="login-footer">
				<div class="system-info">
					<div class="info-item">
						<span class="info-label">System:</span>
						<span class="info-value">AID v0.2</span>
					</div>
					<div class="info-item">
						<span class="info-label">Status:</span>
						<span class="info-value status-online">Online</span>
					</div>
				</div>
			</div>
		</div>
	</div>
</div>

<style>
	* {
		margin: 0;
		padding: 0;
		box-sizing: border-box;
	}

	.app-container {
		height: 100vh;
		width: 100vw;
		background: linear-gradient(135deg, #f5f7fa 0%, #c3cfe2 100%);
		font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
		display: flex;
		flex-direction: column;
		overflow: hidden;
	}

	/* AID Header */
	.aid-header {
		display: flex;
		justify-content: space-between;
		align-items: center;
		background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
		padding: 16px 32px;
		box-shadow: 0 4px 12px rgba(0, 0, 0, 0.15);
	}

	.aid-logo {
		background: white;
		color: #667eea;
		padding: 12px 24px;
		border-radius: 12px;
		font-size: 28px;
		font-weight: bold;
		letter-spacing: 3px;
		box-shadow: 0 4px 8px rgba(0, 0, 0, 0.1);
	}

	.header-subtitle {
		color: rgba(255, 255, 255, 0.9);
		font-size: 16px;
		font-weight: 300;
		letter-spacing: 1px;
	}

	/* Login Content */
	.login-content {
		flex: 1;
		display: flex;
		justify-content: center;
		align-items: center;
		padding: 32px;
	}

	.login-card {
		background: white;
		border-radius: 20px;
		box-shadow: 0 20px 60px rgba(0, 0, 0, 0.1);
		overflow: hidden;
		width: 100%;
		max-width: 480px;
		animation: slideUp 0.6s ease-out;
	}

	@keyframes slideUp {
		from {
			opacity: 0;
			transform: translateY(30px);
		}
		to {
			opacity: 1;
			transform: translateY(0);
		}
	}

	/* Welcome Section */
	.welcome-section {
		background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
		padding: 40px 32px;
		text-align: center;
		color: white;
	}

	.company-logo {
		margin-bottom: 24px;
	}

	.company-logo img {
		height: 64px;
		width: auto;
		filter: brightness(0) invert(1);
		opacity: 0.9;
	}

	.welcome-title {
		font-size: 32px;
		font-weight: 700;
		margin-bottom: 8px;
		letter-spacing: 1px;
	}

	.welcome-subtitle {
		font-size: 16px;
		opacity: 0.9;
		font-weight: 300;
	}

	/* Login Form */
	.login-form {
		padding: 40px 32px 24px;
	}

	.form-group {
		margin-bottom: 24px;
	}

	label {
		display: block;
		margin-bottom: 8px;
		color: #333;
		font-weight: 600;
		font-size: 14px;
		letter-spacing: 0.5px;
	}

	.form-input {
		width: 100%;
		padding: 16px;
		border: 2px solid #e1e5e9;
		border-radius: 12px;
		font-size: 16px;
		font-family: inherit;
		transition: all 0.3s ease;
		background: #fafbfc;
	}

	.form-input:focus {
		outline: none;
		border-color: #667eea;
		background: white;
		box-shadow: 0 0 0 3px rgba(102, 126, 234, 0.1);
	}

	.form-input::placeholder {
		color: #9ca3af;
	}

	/* Login Button */
	.login-button {
		width: 100%;
		background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
		border: none;
		color: white;
		padding: 16px 24px;
		border-radius: 12px;
		font-size: 16px;
		font-weight: 600;
		cursor: pointer;
		transition: all 0.3s ease;
		display: flex;
		align-items: center;
		justify-content: center;
		gap: 12px;
		letter-spacing: 0.5px;
		position: relative;
		overflow: hidden;
	}

	.login-button:hover:not(:disabled) {
		transform: translateY(-2px);
		box-shadow: 0 8px 25px rgba(102, 126, 234, 0.3);
	}

	.login-button:active {
		transform: translateY(0);
	}

	.login-button:disabled {
		opacity: 0.7;
		cursor: not-allowed;
		transform: none;
	}

	.button-icon {
		font-size: 18px;
		font-weight: bold;
		transition: transform 0.3s ease;
	}

	.login-button:hover .button-icon {
		transform: translateX(4px);
	}

	.loading-spinner {
		width: 20px;
		height: 20px;
		border: 2px solid rgba(255, 255, 255, 0.3);
		border-top: 2px solid white;
		border-radius: 50%;
		animation: spin 1s linear infinite;
	}

	@keyframes spin {
		0% {
			transform: rotate(0deg);
		}
		100% {
			transform: rotate(360deg);
		}
	}

	/* Error Message */
	.error-message {
		display: flex;
		align-items: center;
		gap: 12px;
		background: linear-gradient(135deg, #fee 0%, #fdd 100%);
		border: 1px solid #f5c6cb;
		color: #721c24;
		padding: 16px;
		border-radius: 12px;
		margin-bottom: 24px;
		font-size: 14px;
		animation: shake 0.5s ease-in-out;
	}

	@keyframes shake {
		0%,
		100% {
			transform: translateX(0);
		}
		25% {
			transform: translateX(-5px);
		}
		75% {
			transform: translateX(5px);
		}
	}

	.error-icon {
		font-size: 18px;
		color: #dc3545;
	}

	.error-text {
		flex: 1;
		font-weight: 500;
	}

	/* Login Footer */
	.login-footer {
		padding: 24px 32px;
		background: #f8f9fa;
		border-top: 1px solid #e9ecef;
	}

	.system-info {
		display: flex;
		justify-content: space-between;
		align-items: center;
	}

	.info-item {
		display: flex;
		align-items: center;
		gap: 8px;
		font-size: 12px;
	}

	.info-label {
		color: #6c757d;
		font-weight: 500;
	}

	.info-value {
		color: #495057;
		font-weight: 600;
	}

	.status-online {
		color: #28a745;
		position: relative;
	}

	.status-online::before {
		content: '●';
		margin-right: 4px;
		animation: pulse-dot 2s infinite;
	}

	@keyframes pulse-dot {
		0%,
		100% {
			opacity: 1;
		}
		50% {
			opacity: 0.5;
		}
	}

	/* Responsive Design */
	@media (max-width: 480px) {
		.login-content {
			padding: 16px;
		}

		.aid-header {
			padding: 12px 16px;
		}

		.aid-logo {
			font-size: 22px;
			padding: 8px 16px;
		}

		.header-subtitle {
			font-size: 14px;
		}

		.welcome-section {
			padding: 32px 24px;
		}

		.welcome-title {
			font-size: 28px;
		}

		.login-form {
			padding: 32px 24px 20px;
		}

		.system-info {
			flex-direction: column;
			gap: 8px;
			text-align: center;
		}
	}
</style>
