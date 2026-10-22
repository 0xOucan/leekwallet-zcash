/**
 * The board's network and the app's.
 *
 * The board answers for one Zcash network, its Settings -> Network, and
 * refuses a request for the other one with 0x0528 before deriving anything
 * (firmware/docs/PROTOCOL.md, "The board's network"). The Companion is one
 * app for both networks, so that refusal has a one-tap answer: switch the
 * app to the board's network, and read again.
 *
 * Firmware that also reports its network in getStatus (`network`) lets the
 * app follow the board on connect, before anything is refused. Older
 * firmware has no such field; everything here works without it.
 *
 * No DOM: tested under Node (test/companion-flow.test.ts) and against zdev.
 */

import type { ZNetwork } from "../packages/core/src/zcash.ts";

/** The board's refusal of a request for the other network. */
export const WRONG_NETWORK = 0x0528;

export const otherNetwork = (n: ZNetwork): ZNetwork => (n === "main" ? "test" : "main");

const label = (n: ZNetwork): string => (n === "main" ? "Mainnet" : "Testnet");

/** True for the board's 0x0528, whatever wrapped it. */
export function isWrongNetwork(e: unknown): boolean {
  return typeof e === "object" && e !== null && (e as { code?: unknown }).code === WRONG_NETWORK;
}

/**
 * The network named by a getStatus `network` field, or null when the field
 * is absent or says something this app does not know. Accepts the
 * protocol's own "main"/"test" and the spelled-out names, in any case.
 */
export function networkFromStatus(v: unknown): ZNetwork | null {
  if (typeof v !== "string") return null;
  const s = v.trim().toLowerCase();
  if (s === "main" || s === "mainnet") return "main";
  if (s === "test" || s === "testnet") return "test";
  return null;
}

/** What the wrong-network notice says, and its button. */
export interface WrongNetwork {
  /** The network the board is on: the other one, as there are two. */
  board: ZNetwork;
  text: string;
  button: string;
}

/**
 * The notice for a 0x0528 on a request made for `app`. The board's own
 * message is kept after ours: it names its setting in its own words.
 */
export function wrongNetwork(app: ZNetwork, boardMessage?: string): WrongNetwork {
  const board = otherNetwork(app);
  const said = (boardMessage ?? "").trim()
    .replace(/^device error 0x[0-9a-f]{4}:\s*/i, "")
    .replace(/[.\s]+$/, "");
  const own = said ? ` (The board says: ${said}.)` : "";
  return {
    board,
    text:
      `The board is set to ${label(board)} and this app to ${label(app)}, so the board refused and ` +
      `nothing was read. Switch the app to ${label(board)}, or change Settings → Network on the board.` +
      own,
    button: `Switch the app to ${label(board)}`,
  };
}
