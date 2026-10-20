/**
 * ZIP 321 payment requests (`zcash:` URIs) and bare Zcash addresses, as a
 * scanned QR code or a pasted link brings them to the Send form.
 *
 *   zcash:<address>?amount=1.5&memo=<base64url>&label=..&message=..
 *   zcash:?address=<a>&amount=..&address.1=<b>&amount.1=..   (several payments)
 *
 * Parsed strictly, as the ZIP says a wallet must: a parameter given twice,
 * an index with a leading zero, an amount with more than 8 decimals or
 * above the 21 million ZEC supply, a memo for a transparent address, or an
 * unknown `req-` parameter rejects the whole request rather than paying a
 * guess. The address's network is only classified here (from its prefix);
 * the wallet checks the address itself (zw_check_address) before a spend.
 *
 * Pure: no DOM, no Node, so it is tested under plain Node and runs in the
 * webview unchanged.
 */

import { networkOf, type ZNetwork, ZAT_PER_ZEC } from "./zcash.ts";

export interface Zip321Payment {
  address: string;
  /** ZEC as the request wrote it, normalised ("1.50000000" -> "1.5"); null when absent. */
  amount: string | null;
  /** The same amount in zatoshi; null when absent. */
  amountZat: number | null;
  /** The memo as UTF-8 text; null when absent. */
  memo: string | null;
  label: string | null;
  message: string | null;
}

export interface Zip321Request {
  /** The network every address of the request belongs to. */
  network: ZNetwork;
  payments: Zip321Payment[];
  /** True for a bare address (no `zcash:` scheme). */
  bare: boolean;
}

/** Thrown for anything that is not a request this wallet can pay. */
export class Zip321Error extends Error {}

/** 21 million ZEC, the most an amount can be. */
const MAX_ZAT = 21_000_000 * ZAT_PER_ZEC;
const MEMO_MAX = 512;

/**
 * Which kind of address `a` is, from its prefix, and its network; null if
 * it is not a Zcash address. TEX addresses (ZIP 320) are transparent.
 */
export function addressKind(a: string): { network: ZNetwork; kind: "unified" | "sapling" | "transparent" | "tex" } | null {
  if (/^tex1[02-9ac-hj-np-z]+$/.test(a)) return { network: "main", kind: "tex" };
  if (/^textest1[02-9ac-hj-np-z]+$/.test(a)) return { network: "test", kind: "tex" };
  const n = networkOf(a);
  if (!n) return null;
  if (/^(u|utest)1[02-9ac-hj-np-z]+$/.test(a)) return { network: n, kind: "unified" };
  if (/^(zs|ztestsapling)1[02-9ac-hj-np-z]+$/.test(a)) return { network: n, kind: "sapling" };
  if (/^t[13m2]/.test(a)) return { network: n, kind: "transparent" };
  return null; // a viewing key, not an address
}

/** "1.50000000" -> 150000000 zatoshi, exactly; throws on anything else. */
export function parseZecAmount(s: string): { text: string; zat: number } {
  const m = /^(\d+)(?:\.(\d{1,8}))?$/.exec(s);
  if (!m) throw new Zip321Error(`"${s}" is not an amount of ZEC (digits, at most 8 decimals)`);
  const whole = m[1]!.replace(/^0+(?=\d)/, "");
  const frac = (m[2] ?? "").padEnd(8, "0");
  if (whole.length > 8) throw new Zip321Error(`${s} ZEC is more than there will ever be`);
  const zat = Number(whole) * ZAT_PER_ZEC + Number(frac);
  if (zat > MAX_ZAT) throw new Zip321Error(`${s} ZEC is more than there will ever be`);
  const trimmed = frac.replace(/0+$/, "");
  return { text: trimmed ? `${whole}.${trimmed}` : whole, zat };
}

/** Base64url without padding (RFC 4648 section 5), as ZIP 321 memos are. */
function base64url(s: string): Uint8Array {
  if (!/^[A-Za-z0-9_-]*$/.test(s) || s.length % 4 === 1) throw new Zip321Error("the memo is not base64url");
  const std = s.replace(/-/g, "+").replace(/_/g, "/");
  const bin = atob(std + "=".repeat((4 - (std.length % 4)) % 4));
  return Uint8Array.from(bin, (c) => c.charCodeAt(0));
}

function decodeMemo(s: string): string {
  const bytes = base64url(s);
  if (bytes.length > MEMO_MAX) throw new Zip321Error(`the memo is ${bytes.length} bytes; a memo holds at most ${MEMO_MAX}`);
  /* ZIP 302: a first byte of 0xF5 or above is not text. This wallet sends
   * text memos only, so such a request is refused rather than mangled. */
  if (bytes.length > 0 && bytes[0]! >= 0xf5) throw new Zip321Error("the memo is binary data, not text; this wallet sends text memos only");
  try {
    return new TextDecoder("utf-8", { fatal: true }).decode(bytes);
  } catch {
    throw new Zip321Error("the memo is not valid UTF-8 text");
  }
}

function pct(s: string): string {
  try {
    return decodeURIComponent(s);
  } catch {
    throw new Zip321Error(`"${s}" is not valid percent-encoding`);
  }
}

/**
 * Parse what a QR code or a paste holds: a `zcash:` URI or a bare address.
 * Throws a Zip321Error with a sentence for anything else.
 */
export function parsePaymentRequest(raw: string): Zip321Request {
  const text = raw.trim();
  if (!/^zcash:/i.test(text)) {
    const k = addressKind(text);
    if (!k) throw new Zip321Error("This is not a Zcash address or a zcash: payment request.");
    return { network: k.network, bare: true, payments: [{ address: text, amount: null, amountZat: null, memo: null, label: null, message: null }] };
  }
  const rest = text.slice(6);
  const q = rest.indexOf("?");
  const path = q < 0 ? rest : rest.slice(0, q);
  const query = q < 0 ? "" : rest.slice(q + 1);

  /* index -> name -> value. Index 0 is the parameter without a suffix. */
  const byIndex = new Map<number, Map<string, string>>();
  const put = (i: number, name: string, value: string) => {
    let m = byIndex.get(i);
    if (!m) byIndex.set(i, (m = new Map()));
    if (m.has(name)) throw new Zip321Error(`"${name}${i ? `.${i}` : ""}" appears twice`);
    m.set(name, value);
  };
  if (path) put(0, "address", pct(path));
  if (query) {
    for (const part of query.split("&")) {
      if (!part) throw new Zip321Error("an empty parameter");
      const eq = part.indexOf("=");
      if (eq <= 0) throw new Zip321Error(`"${part}" has no value`);
      const key = part.slice(0, eq);
      const value = part.slice(eq + 1);
      const m = /^([A-Za-z0-9+-]+)(?:\.([1-9]\d{0,3}))?$/.exec(key);
      if (!m) throw new Zip321Error(`"${key}" is not a valid parameter name`);
      const name = m[1]!;
      const index = m[2] ? Number(m[2]) : 0;
      if (path && name === "address" && index === 0) throw new Zip321Error("the address is given twice");
      put(index, name, value);
    }
  }
  if (byIndex.size === 0) throw new Zip321Error("the request names no payment");

  let network: ZNetwork | null = null;
  const payments: Zip321Payment[] = [];
  for (const i of [...byIndex.keys()].sort((a, b) => a - b)) {
    const p = byIndex.get(i)!;
    const where = i ? ` (payment ${i})` : "";
    const address = p.get("address");
    if (!address) throw new Zip321Error(`a payment without an address${where}`);
    const kind = addressKind(address);
    if (!kind) throw new Zip321Error(`"${address}" is not a Zcash address${where}`);
    if (network && kind.network !== network) throw new Zip321Error("the request mixes mainnet and testnet addresses");
    network = kind.network;
    let amount: { text: string; zat: number } | null = null;
    let memo: string | null = null;
    let label: string | null = null;
    let message: string | null = null;
    for (const [name, value] of p) {
      switch (name) {
        case "address": break;
        case "amount": amount = parseZecAmount(value); break;
        case "memo":
          if (kind.kind === "transparent" || kind.kind === "tex") {
            throw new Zip321Error(`a memo for a transparent address${where}: only a shielded recipient can read one`);
          }
          memo = decodeMemo(value);
          break;
        case "label": label = pct(value); break;
        case "message": message = pct(value); break;
        default:
          if (name.startsWith("req-")) throw new Zip321Error(`the request requires "${name}", which this wallet does not support`);
          /* Any other parameter is optional by the ZIP's rule: ignored. */
      }
    }
    payments.push({ address, amount: amount?.text ?? null, amountZat: amount?.zat ?? null, memo, label, message });
  }
  return { network: network!, payments, bare: false };
}
