/**
 * The last board this app paired with: its link (USB or Bluetooth) and,
 * for Bluetooth, the id and advertised name, so the launcher can offer a
 * one-tap reconnect to it.
 *
 * Nothing secret is kept. The session is a fresh X25519 handshake on every
 * connection, compared by passkey and approved on the board (main.ts's
 * Client); no key of it outlives the connection, and none is stored here.
 * The id is what the OS reports for the peer (a MAC address on Android, a
 * platform UUID on a desktop), the name is the board's own advertisement.
 *
 * Storage is injected so the tests run under plain Node.
 */

export interface RememberedBoard {
  link: "usb" | "ble";
  /** Bluetooth only: the peer id ble_connect takes. */
  bleId?: string;
  /** Bluetooth only: what the board advertised, for the button's label. */
  bleName?: string | null;
  /** When it was last paired (ms since the epoch). */
  at: number;
}

export const BOARD_KEY = "leek.lastBoard";

type Store = Pick<Storage, "getItem" | "setItem" | "removeItem">;

function store(s?: Store): Store | null {
  if (s) return s;
  try { return globalThis.localStorage ?? null; } catch { return null; }
}

/** The remembered board, or null when none (or nothing readable) was kept. */
export function loadBoard(s?: Store): RememberedBoard | null {
  try {
    const raw = store(s)?.getItem(BOARD_KEY);
    if (!raw) return null;
    const v = JSON.parse(raw) as Partial<RememberedBoard>;
    if (v.link === "usb") return { link: "usb", at: Number(v.at) || 0 };
    if (v.link === "ble" && typeof v.bleId === "string" && v.bleId.length > 0 && v.bleId.length <= 128) {
      const name = typeof v.bleName === "string" ? v.bleName.slice(0, 64) : null;
      return { link: "ble", bleId: v.bleId, bleName: name, at: Number(v.at) || 0 };
    }
    return null;
  } catch {
    return null;
  }
}

export function saveBoard(b: RememberedBoard, s?: Store): void {
  try { store(s)?.setItem(BOARD_KEY, JSON.stringify(b)); } catch { /* remembered for this run only */ }
}

export function forgetBoard(s?: Store): void {
  try { store(s)?.removeItem(BOARD_KEY); } catch { /* nothing to forget */ }
}

/** "Bluetooth · LeekWallet-1A2B" or "USB cable", for the reconnect button. */
export function boardLabel(b: RememberedBoard): string {
  if (b.link === "usb") return "USB cable";
  return `Bluetooth · ${b.bleName || b.bleId}`;
}
