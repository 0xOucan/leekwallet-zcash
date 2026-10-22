/**
 * How a transaction of the wallet's history is named (Home -> Recent
 * activity, and Activity).
 *
 * The kinds are zecz's (host/zecz-core TxKind) as the desktop backend's
 * history names them (app/zcash-wallet/src/backend.rs, tx_dto): received,
 * sent, self (the account paid itself: every output was its own, and what
 * it lost was the fee), shielded and internal. A kind added there later
 * reads as its own name, or "Transaction", never "undefined" (the label a
 * send to yourself had before "self" was known here).
 *
 * No DOM: tested under Node (test/companion-flow.test.ts).
 */

import { formatZec } from "../packages/core/src/zcash.ts";

const KNOWN: Record<string, string> = {
  received: "Received",
  sent: "Sent",
  self: "Sent to yourself",
  shielded: "Shielded",
  internal: "Moved between pools",
};

/** The label of a history kind; readable for a kind this app has not met. */
export function txKindLabel(kind: unknown): string {
  if (typeof kind !== "string") return "Transaction";
  const known = KNOWN[kind];
  if (known) return known;
  const words = kind.replace(/[_-]+/g, " ").replace(/([a-z])([A-Z])/g, "$1 $2").trim().toLowerCase();
  if (!words || !/^[a-z0-9 ]{1,40}$/.test(words)) return "Transaction";
  return words[0]!.toUpperCase() + words.slice(1);
}

/** What a history row shows in its head: the label and the amount. */
export interface TxHead {
  label: string;
  /** "−1.5 ZEC", "+0.1 ZEC", "−0.0001 ZEC fee"; masked when balances are hidden. */
  amount: string;
  /** For styling: value left the account or came in. */
  dir: "in" | "out";
}

export interface TxLike {
  kind: unknown;
  deltaZat: number;
  amountZat: number;
  feeZat: number | null;
}

/**
 * The head of a history row. A send to yourself moved nothing to anyone, so
 * its amount is what it cost, the fee, and says so.
 */
export function txHead(t: TxLike, hide = false): TxHead {
  const label = txKindLabel(t.kind);
  const dir = t.deltaZat < 0 ? "out" : "in";
  if (hide) return { label, amount: "••••", dir };
  if (t.kind === "self") {
    const fee = t.feeZat ?? Math.abs(t.deltaZat);
    return { label, amount: `−${formatZec(fee)} ZEC fee`, dir: "out" };
  }
  return { label, amount: `${t.deltaZat < 0 ? "−" : "+"}${formatZec(Math.abs(t.deltaZat))} ZEC`, dir };
}
