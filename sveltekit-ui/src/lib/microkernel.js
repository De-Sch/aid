/**
 * @file microkernel.js
 * @description Helper utilities for spawning and communicating with the AID microkernel
 *
 * This module provides common functionality for SvelteKit API endpoints to interact
 * with the C++ microkernel binary. It handles both direct execution and CGI proxy modes.
 *
 * Execution Modes:
 * 1. Direct Binary Execution (spawn):
 *    - Used for most operations (dashboard, close, comment)
 *    - Spawns microkernel with command line arguments
 *    - Reads JSON from stdout, avoids logging interference
 *
 * 2. CGI Proxy (fetch):
 *    - Used for POST operations that need CGI environment
 *    - Proxies requests to Apache CGI endpoint
 *    - Maintains compatibility with legacy CGI infrastructure
 *
 * @see /home/user/projects/aid/microkernel/main.cpp
 */

import { spawn } from 'child_process';

/**
 * Microkernel configuration paths
 * TODO: Make these configurable via environment variables
 */
const MICROKERNEL_PATHS = {
	binary: '/home/user/projects/aid/build/microkernel/microkernel',
	config: '/home/user/projects/aid/microkernel/include/config.json',
	cgiBase: 'http://localhost/cgi-bin/user/gui'
};

/**
 * Spawn microkernel process and collect output
 * @param {string} urlPath - URL path for routing (e.g., "/ui/dashboard/user")
 * @param {Object} options - Spawn options
 * @param {string} [options.stdinData] - Optional data to send to stdin
 * @param {Object} [options.env] - Additional environment variables
 * @returns {Promise<{stdout: string, stderr: string, code: number}>}
 *
 * @example
 * const result = await spawnMicrokernel('/ui/dashboard/alice');
 * const data = JSON.parse(result.stdout);
 */
export async function spawnMicrokernel(urlPath, options = {}) {
	const { stdinData, env } = options;

	// Set up environment for microkernel
	const processEnv = {
		...process.env,
		REQUEST_METHOD: stdinData ? 'POST' : 'GET',
		REQUEST_URI: urlPath,
		PATH_INFO: urlPath,
		...env
	};

	// Spawn microkernel with config and URL path
	const microkernelProcess = spawn(MICROKERNEL_PATHS.binary, [MICROKERNEL_PATHS.config, urlPath], {
		env: processEnv
	});

	let stdout = '';
	let stderr = '';

	// Collect output streams
	microkernelProcess.stdout.on('data', (data) => {
		stdout += data.toString();
	});

	microkernelProcess.stderr.on('data', (data) => {
		stderr += data.toString();
	});

	// Send stdin data if provided
	if (stdinData) {
		microkernelProcess.stdin.write(stdinData);
	}
	microkernelProcess.stdin.end();

	// Wait for process to complete
	const code = await new Promise((resolve, reject) => {
		microkernelProcess.on('close', (exitCode) => {
			resolve(exitCode);
		});

		microkernelProcess.on('error', (error) => {
			reject(error);
		});
	});

	return { stdout, stderr, code };
}

/**
 * Execute microkernel and extract JSON from stdout
 * @param {string} urlPath - URL path for routing
 * @param {Object} options - Execution options
 * @param {string} [options.stdinData] - Optional stdin data
 * @param {Object} [options.env] - Additional environment variables
 * @returns {Promise<Object>} Parsed JSON response
 * @throws {Error} If JSON parsing fails or microkernel returns error
 *
 * @example
 * const dashboard = await executeMicrokernelJSON('/ui/dashboard/bob');
 * console.log(dashboard.tickets);
 */
export async function executeMicrokernelJSON(urlPath, options = {}) {
	const { stdout, stderr, code } = await spawnMicrokernel(urlPath, options);

	console.log('Microkernel exit code:', code);
	console.log('Microkernel stdout length:', stdout.length);
	if (stderr) {
		console.log('Microkernel stderr:', stderr);
	}

	// Extract JSON from stdout
	// The microkernel may output headers or logging before JSON
	const jsonData = extractJSONFromOutput(stdout);

	if (!jsonData) {
		throw new Error('Failed to extract JSON from microkernel output');
	}

	return jsonData;
}

/**
 * Extract JSON object from microkernel stdout
 * @param {string} stdout - Raw stdout from microkernel
 * @returns {Object|null} Parsed JSON or null if extraction failed
 * @private
 *
 * Handles various output formats:
 * - Pure JSON (starts with '{')
 * - HTTP headers followed by JSON (finds first '{' after headers)
 * - Multiple JSON objects (returns first valid one)
 */
function extractJSONFromOutput(stdout) {
	const lines = stdout.split('\n');

	// Try to find JSON start
	let jsonStartIndex = -1;

	for (let i = 0; i < lines.length; i++) {
		const line = lines[i].trim();

		// Skip empty lines and HTTP headers
		if (line === '' || line.includes(':')) {
			continue;
		}

		// Found potential JSON start
		if (line.startsWith('{') || line.startsWith('[')) {
			jsonStartIndex = i;
			break;
		}
	}

	if (jsonStartIndex === -1) {
		return null;
	}

	// Extract all lines from JSON start to end
	const jsonLines = lines.slice(jsonStartIndex).filter((line) => line.trim() !== '');
	const jsonString = jsonLines.join('\n');

	try {
		return JSON.parse(jsonString);
	} catch (error) {
		console.error('JSON parse error:', error);
		console.log('Failed JSON string:', jsonString);
		return null;
	}
}

/**
 * Proxy request to CGI endpoint via HTTP
 * @param {string} urlPath - URL path for routing
 * @param {Object} options - Fetch options
 * @param {string} [options.method='GET'] - HTTP method
 * @param {string} [options.body] - Request body
 * @returns {Promise<Response>} SvelteKit Response object
 *
 * @example
 * const response = await proxyCGI('/ui/dashboard/charlie', {
 *     method: 'POST',
 *     body: JSON.stringify({ action: 'refresh' })
 * });
 */
export async function proxyCGI(urlPath, options = {}) {
	const { method = 'GET', body } = options;
	const cgiUrl = `${MICROKERNEL_PATHS.cgiBase}${urlPath}`;

	const response = await fetch(cgiUrl, {
		method,
		headers: {
			'Content-Type': 'application/json'
		},
		body
	});

	const data = await response.text();

	return new Response(data, {
		status: response.status,
		headers: {
			'Content-Type': 'application/json',
			'Access-Control-Allow-Origin': '*'
		}
	});
}

/**
 * Check if microkernel output indicates success
 * @param {Object} jsonResponse - Parsed JSON response from microkernel
 * @returns {boolean} True if operation succeeded
 *
 * Checks for standard success indicators:
 * - status: 'SUCCESS'
 * - success: true
 * - No error field
 */
export function isSuccessResponse(jsonResponse) {
	if (!jsonResponse) return false;

	return jsonResponse.status === 'SUCCESS' || jsonResponse.success === true || !jsonResponse.error;
}

/**
 * Create standardized error response
 * @param {string} message - Error message
 * @param {Object} [details] - Additional error details
 * @returns {Response} SvelteKit Response with error JSON
 */
export function createErrorResponse(message, details = {}) {
	return new Response(
		JSON.stringify({
			error: message,
			...details
		}),
		{
			status: 500,
			headers: {
				'Content-Type': 'application/json',
				'Access-Control-Allow-Origin': '*'
			}
		}
	);
}

/**
 * Create standardized success response
 * @param {Object} data - Response data
 * @returns {Response} SvelteKit Response with success JSON
 */
export function createSuccessResponse(data) {
	return new Response(JSON.stringify(data), {
		status: 200,
		headers: {
			'Content-Type': 'application/json',
			'Access-Control-Allow-Origin': '*'
		}
	});
}
