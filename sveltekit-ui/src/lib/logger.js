/**
 * @file logger.js
 * @description Frontend logger utility for AID system
 *
 * Provides a centralized logging interface for the SvelteKit frontend that forwards
 * log messages to the backend microkernel for proper logging infrastructure.
 *
 * Architecture:
 * - Frontend components use this logger instead of console.log
 * - Logger sends messages to /api/log endpoint
 * - Backend microkernel writes to unified log file
 * - Currently API calls are disabled to avoid debugger breakpoint issues
 *
 * Temporary State:
 * API logging is currently disabled (see sendLog method) due to debugger
 * breakpoint issues. Messages fall back to console.log for now.
 *
 * @see /home/user/projects/aid/sveltekit-ui/src/routes/api/log/+server.js
 */

/**
 * @class FrontendLogger
 * @description Singleton logger for frontend components
 *
 * Provides standard log levels (TRACE, DEBUG, INFO, WARN, ERROR, FATAL)
 * and component-specific logging through the setComponent method.
 *
 * Usage:
 * ```javascript
 * import { logger } from '$lib/logger';
 * logger.setComponent('Dashboard');
 * logger.info('User logged in');
 * ```
 */
class FrontendLogger {
	constructor() {
		/** @type {string} Component name for log context */
		this.component = 'Unknown';
	}

	/**
	 * Set the component name for log context
	 * @param {string} componentName - Name of the component using this logger
	 * @returns {FrontendLogger} This logger instance for method chaining
	 */
	setComponent(componentName) {
		this.component = componentName;
		return this;
	}

	/**
	 * Send log message to backend microkernel
	 * @param {string} level - Log level (TRACE, DEBUG, INFO, WARN, ERROR, FATAL)
	 * @param {string|object} message - Message to log
	 * @private
	 *
	 * Note: API calls are temporarily disabled to fix breakpoint issue.
	 * Falls back to console.log until backend logging is re-enabled.
	 */
	async sendLog(level, message) {
		try {
			// Temporarily disabled to fix breakpoint issue
			// await fetch('/api/log', {
			//     method: 'POST',
			//     headers: {
			//         'Content-Type': 'application/json',
			//     },
			//     body: JSON.stringify({
			//         level,
			//         message: String(message),
			//         component: this.component,
			//         timestamp: new Date().toISOString()
			//     })
			// });
			// Use console.log for now
			console.log(`[${level}] [${this.component}]`, message);
		} catch (error) {
			// Fallback to console if API fails
			console.error('Logger API failed:', error);
			console.log(`[${level}] [${this.component}]`, message);
		}
	}

	/**
	 * Log at TRACE level (most verbose)
	 * @param {string|object} message - Message to log
	 */
	trace(message) {
		this.sendLog('TRACE', message);
	}

	/**
	 * Log at DEBUG level (detailed diagnostic info)
	 * @param {string|object} message - Message to log
	 */
	debug(message) {
		this.sendLog('DEBUG', message);
	}

	/**
	 * Log at INFO level (general information)
	 * @param {string|object} message - Message to log
	 */
	info(message) {
		this.sendLog('INFO', message);
	}

	/**
	 * Log at WARN level (warning conditions)
	 * @param {string|object} message - Message to log
	 */
	warn(message) {
		this.sendLog('WARN', message);
	}

	/**
	 * Log at ERROR level (error conditions)
	 * @param {string|object} message - Message to log
	 */
	error(message) {
		this.sendLog('ERROR', message);
	}

	/**
	 * Log at FATAL level (fatal errors, system unusable)
	 * @param {string|object} message - Message to log
	 */
	fatal(message) {
		this.sendLog('FATAL', message);
	}
}

/**
 * Singleton logger instance for default usage
 * @type {FrontendLogger}
 * @example
 * import { logger } from '$lib/logger';
 * logger.setComponent('MyComponent');
 * logger.info('Something happened');
 */
export const logger = new FrontendLogger();

/**
 * Factory function to create component-specific loggers
 * @param {string} componentName - Name of the component
 * @returns {FrontendLogger} New logger instance with component name set
 * @example
 * import { createLogger } from '$lib/logger';
 * const log = createLogger('Dashboard');
 * log.debug('Dashboard initialized');
 */
export function createLogger(componentName) {
	return new FrontendLogger().setComponent(componentName);
}
