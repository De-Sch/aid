/**
 * Toast event channel — a tiny, transport-free notification queue.
 *
 * Anything in the app can `toasts.push(...)` a transient message; UI components
 * (built in later prompts) read `toasts.items` reactively and render them. Toasts
 * auto-dismiss after a timeout, or can be dismissed early. Runes module
 * (`.svelte.ts`): the `$state` below is shared app-wide.
 *
 * The dashboard store feeds this from `action_result` stream frames; it carries
 * no opinion about styling — only `kind` + `message`.
 */

/** Visual category for a toast; the component maps it to a colour/icon. */
export type ToastKind = 'success' | 'error' | 'info';

/** One queued toast. `id` is unique and monotonic for keying + dismissal. */
export interface Toast {
	id: number;
	kind: ToastKind;
	message: string;
}

/** What callers pass to {@link toasts.push}. */
export interface ToastInput {
	kind: ToastKind;
	message: string;
	/** Auto-dismiss delay in ms; default 5000. Pass 0 to keep until dismissed. */
	durationMs?: number;
}

const DEFAULT_DURATION_MS = 5000;

let items = $state<Toast[]>([]);
let nextId = 0;

export const toasts = {
	/** Current live toasts, oldest first. Read reactively in components. */
	get items(): Toast[] {
		return items;
	},

	/**
	 * Queue a toast. Returns its id (for early {@link dismiss}). Schedules
	 * auto-dismiss unless `durationMs` is 0.
	 */
	push({ kind, message, durationMs = DEFAULT_DURATION_MS }: ToastInput): number {
		const id = nextId++;
		items = [...items, { id, kind, message }];
		if (durationMs > 0) {
			setTimeout(() => toasts.dismiss(id), durationMs);
		}
		return id;
	},

	/** Remove a toast by id. No-op if already gone. */
	dismiss(id: number): void {
		items = items.filter((t) => t.id !== id);
	},

	/** Remove all toasts. */
	clear(): void {
		items = [];
	}
};
