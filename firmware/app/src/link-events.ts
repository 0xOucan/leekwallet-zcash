/**
 * The board link across the app's lifecycle, the webview's half
 * (src-tauri/src/link.rs is the other): a link left open by a previous run
 * is closed when the webview starts, and a link the backend closed while
 * the app was in the background ends the session on screen with a reason.
 *
 * The board serves one peer and does not advertise while connected, so a
 * link nobody uses keeps the next connection out; that was "Reconnect only
 * works after restarting the board" (docs/JOURNAL.md, 6.17).
 *
 * No DOM: tested under Node (test/companion-flow.test.ts).
 */

import type { Invoke } from "./tauri-transport.ts";

/** Emitted by the backend when it closed a link (link.rs RELEASED_EVENT). */
export const RELEASED_EVENT = "link://released";

/** Why the backend closed the link, as the device log and the panel say it. */
export function releasedMessage(reason: string): string {
  switch (reason) {
    case "background":
      return "The app was in the background for more than 30 seconds, so the link to the board was closed and the board advertises again. Reconnect your board to go on.";
    case "closing":
      return "The app is closing: the link to the board was closed.";
    case "a new start":
      return "A link to the board left open by the previous run was closed, so the board advertises again.";
    default:
      return `The link to the board was closed (${reason || "no reason given"}). Reconnect your board to go on.`;
  }
}

/**
 * At start: close any link the backend still holds from before this webview
 * (an Android activity reopened in a process that kept the old link).
 * Returns the line to log, or null when there was nothing to close. Never
 * throws: a backend without the command (an older build) is not a reason to
 * stop the app from starting.
 */
export async function closeLeftoverLink(invoke: Invoke | null): Promise<string | null> {
  if (!invoke) return null;
  try {
    const closed = await invoke<boolean>("link_reset");
    return closed ? releasedMessage("a new start") : null;
  } catch (e) {
    return `could not close a previous link: ${String((e as Error)?.message ?? e)}`;
  }
}
