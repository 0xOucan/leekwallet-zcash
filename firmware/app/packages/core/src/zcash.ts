/**
 * The Zcash methods of the board, host side (docs/PROTOCOL.md §4, "Zcash
 * account metadata", "Zcash features and address check", "Zcash signing").
 *
 * Written over a `call` function rather than over the client, so it carries
 * no transport, no session and no DOM: the companion passes its own
 * serialised, encrypted `Client.call`, and the tests pass one that talks to
 * zdev (firmware/sim) over a PTY. The same calls, timeouts and checks as
 * host/leek-device/src/device.rs and its JavaScript twin in
 * chrome/extension/src/board/device.js, so the three hosts cannot drift into
 * reading one reply three ways.
 *
 * The `.zsig` writer is the one in docs/zcash/FORMATS.md, byte for byte what
 * leek-device's zsig.rs writes, so a file saved here is one `zecz finish`,
 * the desktop app and Zodl read.
 */

import { sha256 } from "@noble/hashes/sha256";
import type { CborValue } from "./cbor.ts";
import { DeviceError } from "./transport.ts";

export type ZNetwork = "main" | "test";

/** SHA-256, as the .zsig names a PCZT. Re-exported so the app needs no hash dependency of its own. */
export { sha256 };

/** The companion's request function: one encrypted call, one reply. */
export type ZCall = (
  method: string,
  params?: Record<string, CborValue>,
  timeoutMs?: number,
) => Promise<Record<string, CborValue>>;

/**
 * How long each call may take, waiting longer than the board does (the
 * client's rule: a transport that gives up first leaves the counters apart).
 * Deriving an Orchard account takes seconds at 160 MHz; anything the user
 * answers on the board gets as long as a person reading review pages.
 */
export const Z_TIMEOUT = { short: 10_000, derive: 90_000, user: 150_000 } as const;

/** Error codes of the Zcash calls (PROTOCOL.md §6). */
export const ZErr = {
  UserRejected: 0x0200,
  UserTimeout: 0x0201,
  /** 0x05xx: the board will not sign; the low byte is the reason. */
  SignBase: 0x0500,
  /** ZS_E_OUTPUT_COMMITMENT on zShowAddress: the address is not this wallet's. */
  ForeignAddress: 0x050e,
  /** ZS_E_UNSUPPORTED on zShowAddress: a receiver the board cannot check. */
  UncheckableAddress: 0x0510,
} as const;

/** Pools of a signature (`ZS_POOL_*`). */
export const ZPool = { Transparent: 0, Orchard: 1, Ironwood: 2 } as const;

export interface ZAccount {
  account: number;
  network: ZNetwork;
  /** ZIP 32 seed fingerprint, hex. */
  seedFingerprint: string;
  ufvk: string;
  ua: string;
  tAddress: string;
  /** Per network; null when the board has none for this account. */
  birthday: number | null;
}

export interface ZFeatures {
  backend: string;
  branchIds: number[];
  maxPczt: number;
  maxChunk: number;
  showAddress: boolean;
}

export interface ZSigEntry {
  pool: number;
  index: number;
  sig: Uint8Array;
  /** Transparent entries only: the compressed key the signature verifies under. */
  pubkey?: Uint8Array;
}

export interface ZSigned {
  /** The shielded sighash the user approved on the board. */
  sighash: Uint8Array;
  /** SHA-256 of the exact PCZT bytes sent. */
  sha256: Uint8Array;
  entries: ZSigEntry[];
}

export type ZProgress =
  | { phase: "upload"; sent: number; total: number }
  | { phase: "review" };

export function checkNetwork(n: string): asserts n is ZNetwork {
  if (n !== "main" && n !== "test") throw new Error(`network must be "main" or "test", not ${n}`);
}

export const toHex = (b: Uint8Array): string =>
  Array.from(b, (x) => x.toString(16).padStart(2, "0")).join("");

function bytesField(v: Record<string, CborValue>, key: string, len?: number): Uint8Array {
  const b = v[key];
  if (!(b instanceof Uint8Array)) throw new Error(`the board reply has no byte string "${key}"`);
  if (len !== undefined && b.length !== len) {
    throw new Error(`"${key}" is ${b.length} bytes, expected ${len}`);
  }
  return b;
}

function uintField(v: Record<string, CborValue>, key: string): number {
  const n = v[key];
  if (typeof n !== "number" || !Number.isInteger(n) || n < 0) {
    throw new Error(`the board reply has no integer "${key}"`);
  }
  return n;
}

function textField(v: Record<string, CborValue>, key: string): string {
  const t = v[key];
  if (typeof t !== "string") throw new Error(`the board reply has no text "${key}"`);
  return t;
}

/**
 * Which network an address or viewing key belongs to, from its prefix, or
 * null if it is not one this wallet knows. Bech32m HRPs (ZIP 316) and the
 * transparent Base58Check prefixes: t1/t3 on mainnet, tm/t2 on testnet.
 */
export function networkOf(s: string): ZNetwork | null {
  if (/^(u|uview|uivk|zs|zviews)1/.test(s)) return "main";
  if (/^(utest|uviewtest|uivktest|ztestsapling|zxviewtestsapling)1/.test(s)) return "test";
  if (/^t[13][1-9A-HJ-NP-Za-km-z]{33}$/.test(s)) return "main";
  if (/^t[m2][1-9A-HJ-NP-Za-km-z]{33}$/.test(s)) return "test";
  return null;
}

/** What the firmware can sign and how big a PCZT it takes. Needs no pairing. */
export async function zGetFeatures(call: ZCall): Promise<ZFeatures> {
  const f = await call("zGetFeatures", {}, Z_TIMEOUT.short);
  const ids = f["branchIds"];
  return {
    backend: textField(f, "backend"),
    branchIds: Array.isArray(ids) ? ids.filter((x): x is number => typeof x === "number") : [],
    maxPczt: uintField(f, "maxPczt"),
    maxChunk: uintField(f, "maxChunk"),
    showAddress: f["showAddress"] === 1,
  };
}

/**
 * UFVK, unified and transparent address, seed fingerprint and birthday of
 * one account on one network.
 *
 * The reply is checked against what was asked: a board that answered for
 * the other network (or an account other than the one asked for) is a
 * board this side must not show, because an address of the wrong network
 * reads as valid until someone sends to it.
 */
export async function zGetAccount(call: ZCall, account: number, network: ZNetwork): Promise<ZAccount> {
  checkNetwork(network);
  const a = await call("zGetAccount", { account, network }, Z_TIMEOUT.derive);
  const out: ZAccount = {
    account: uintField(a, "account"),
    network: textField(a, "network") as ZNetwork,
    seedFingerprint: toHex(bytesField(a, "seedFingerprint", 32)),
    ufvk: textField(a, "ufvk"),
    ua: textField(a, "ua"),
    tAddress: textField(a, "tAddress"),
    birthday: typeof a["birthday"] === "number" && Number.isInteger(a["birthday"]) ? a["birthday"] : null,
  };
  if (out.account !== account) throw new Error(`asked for account ${account}, the board answered ${out.account}`);
  if (out.network !== network) throw new Error(`asked for ${network}, the board answered ${out.network}`);
  for (const [what, s] of [["viewing key", out.ufvk], ["address", out.ua], ["t-address", out.tAddress]]) {
    if (networkOf(s!) !== network) {
      throw new Error(`the board's ${what} is not a ${network === "main" ? "mainnet" : "testnet"} one`);
    }
  }
  return out;
}

/** Highest plausible birthday; anything above is a typo, not a block. */
export const MAX_BIRTHDAY = 100_000_000;

/** Record the birthday on the board, per network. True when kept in flash. */
export async function zSetBirthday(
  call: ZCall, account: number, network: ZNetwork, birthday: number,
): Promise<boolean> {
  checkNetwork(network);
  if (!Number.isInteger(birthday) || birthday < 1 || birthday > MAX_BIRTHDAY) {
    throw new Error("the birthday is a block height: a whole number above 0");
  }
  const r = await call("zSetAccountMeta", { account, network, birthday }, Z_TIMEOUT.derive);
  return uintField(r, "stored") === 1;
}

/**
 * The board derives the account's address itself, pages it on its screen and
 * answers only after the user says it matches `ua` (any of the account's
 * diversified addresses). Rejects with a sentence for the codes a person can
 * act on; the board's own message stays in the error.
 */
export async function zShowAddress(
  call: ZCall, account: number, network: ZNetwork, ua: string,
): Promise<{ ua: string; tAddress: string | null }> {
  checkNetwork(network);
  let r: Record<string, CborValue>;
  try {
    r = await call("zShowAddress", { account, network, ua }, Z_TIMEOUT.user);
  } catch (e) {
    if (e instanceof DeviceError) {
      const why =
        e.code === ZErr.ForeignAddress
          ? "The board says this address is NOT from its wallet. Do not use it to receive."
          : e.code === ZErr.UncheckableAddress
            ? "This address has a receiver the board cannot check (for example Sapling), so it was not shown."
            : e.code === ZErr.UserRejected
              ? "You answered on the board that the addresses do not match."
              : e.code === ZErr.UserTimeout
                ? "No answer on the board in time."
                : null;
      if (why) {
        const out = new DeviceError(e.code, e.message);
        out.message = `${why} (${e.message})`;
        throw out;
      }
    }
    throw e;
  }
  const shown = textField(r, "ua");
  /* Firmware without the rotated-address check ignores `ua` and shows its
   * default address: this is what catches the difference. */
  if (shown !== ua) throw new Error(`the board confirmed ${shown}, but this app shows ${ua}`);
  return { ua: shown, tAddress: typeof r["tAddress"] === "string" ? r["tAddress"] : null };
}

/** ZIP 374: "PCZT" and a u32 LE version, then the postcard body. */
export function checkPczt(bytes: Uint8Array): { version: number } {
  if (bytes.length < 8 || bytes[0] !== 0x50 || bytes[1] !== 0x43 || bytes[2] !== 0x5a || bytes[3] !== 0x54) {
    throw new Error("this is not a PCZT file (it does not start with \"PCZT\")");
  }
  const version = (bytes[4]! | (bytes[5]! << 8) | (bytes[6]! << 16) | (bytes[7]! << 24)) >>> 0;
  if (version !== 1 && version !== 2) throw new Error(`PCZT version ${version} is not one the board reads`);
  return { version };
}

/**
 * Upload a PCZT, let the user review it on the board, and return the sighash
 * they approved with the board's signatures.
 *
 * The bytes go as they are. The `.zsig` names the SHA-256 of exactly what was
 * signed, and whoever made the file (zecz, the desktop app, Zodl) checks the
 * signatures against the PCZT it kept: a view redacted here would sign fine
 * and then be refused there. Making the compact signer view is the builder's
 * job, at build time.
 */
export async function zSign(
  call: ZCall, pczt: Uint8Array, account: number, network: ZNetwork,
  onProgress?: (p: ZProgress) => void,
): Promise<ZSigned> {
  checkNetwork(network);
  checkPczt(pczt);
  const sha = sha256(pczt);
  const begin = await call("zSignBegin", { len: pczt.length, sha256: sha, account, network }, Z_TIMEOUT.derive);
  const max = uintField(begin, "maxChunk");
  if (max === 0) throw new Error("the board offered a zero chunk size");
  let off = 0;
  while (off < pczt.length) {
    const n = Math.min(max, pczt.length - off);
    const r = await call("zSignChunk", { offset: off, data: pczt.slice(off, off + n) }, Z_TIMEOUT.short);
    off += n;
    if (uintField(r, "received") !== off) {
      throw new Error(`the board's received count disagrees after ${off} bytes`);
    }
    onProgress?.({ phase: "upload", sent: off, total: pczt.length });
  }
  onProgress?.({ phase: "review" });
  const done = await call("zSignFinish", {}, Z_TIMEOUT.user);
  const sighash = bytesField(done, "sighash", 32);
  const sigs = done["signatures"];
  if (!Array.isArray(sigs)) throw new Error("the board reply has no signatures");
  const entries = sigs.map((raw): ZSigEntry => {
    const s = raw as Record<string, CborValue>;
    const pool = uintField(s, "pool");
    const index = uintField(s, "index");
    if (pool === ZPool.Transparent) {
      const sig = bytesField(s, "sig");
      if (sig.length < 9 || sig.length > 73) throw new Error(`transparent signature of ${sig.length} bytes`);
      return { pool, index, sig, pubkey: bytesField(s, "pubkey", 33) };
    }
    if (pool !== ZPool.Orchard && pool !== ZPool.Ironwood) {
      throw new Error(`the board returned a signature for pool ${pool}, which no host can apply`);
    }
    return { pool, index, sig: bytesField(s, "sig", 64) };
  });
  return { sighash, sha256: sha, entries };
}

/* ------------------------------------------------------------------ .zsig */

const TSIG_MIN = 9;
const TSIG_MAX = 73;

/**
 * The `.zsig` file (docs/zcash/FORMATS.md): "ZSIG", version (1 if every
 * entry is shielded, 2 if any is transparent), SHA-256 of the PCZT, a u16 LE
 * count, then the entries.
 */
export function encodeZsig(pcztSha256: Uint8Array, entries: ZSigEntry[]): Uint8Array {
  if (pcztSha256.length !== 32) throw new Error("the PCZT hash is not 32 bytes");
  if (entries.length > 0xffff) throw new Error("too many signatures for a .zsig");
  const anyT = entries.some((e) => e.pool === ZPool.Transparent);
  const out: number[] = [0x5a, 0x53, 0x49, 0x47, anyT ? 2 : 1, ...pcztSha256,
    entries.length & 0xff, entries.length >> 8];
  for (const e of entries) {
    if (!Number.isInteger(e.index) || e.index < 0 || e.index > 0xffffffff) {
      throw new Error(`bad signature index ${e.index}`);
    }
    out.push(e.pool, e.index & 0xff, (e.index >>> 8) & 0xff, (e.index >>> 16) & 0xff, e.index >>> 24);
    if (e.pool === ZPool.Transparent) {
      if (e.sig.length < TSIG_MIN || e.sig.length > TSIG_MAX) {
        throw new Error(`transparent signature of ${e.sig.length} bytes`);
      }
      if (!e.pubkey || e.pubkey.length !== 33) throw new Error("transparent signature without its 33-byte key");
      out.push(e.sig.length, ...e.sig, ...e.pubkey);
    } else if (e.pool === ZPool.Orchard || e.pool === ZPool.Ironwood) {
      if (e.sig.length !== 64) throw new Error(`shielded signature of ${e.sig.length} bytes`);
      out.push(...e.sig);
    } else {
      throw new Error(`unknown pool ${e.pool}`);
    }
  }
  return Uint8Array.from(out);
}

/** The reverse, for tests and for checking a file before it is handed over. */
export function parseZsig(bytes: Uint8Array): { version: number; pcztSha256: Uint8Array; entries: ZSigEntry[] } {
  const fail = (why: string): never => { throw new Error(`.zsig: ${why}`); };
  if (bytes.length < 39) fail("truncated");
  if (String.fromCharCode(...bytes.slice(0, 4)) !== "ZSIG") fail("bad magic");
  const version = bytes[4]!;
  if (version !== 1 && version !== 2) fail(`version ${version}`);
  const pcztSha256 = bytes.slice(5, 37);
  const n = bytes[37]! | (bytes[38]! << 8);
  let at = 39;
  const take = (k: number): Uint8Array => {
    if (at + k > bytes.length) fail("truncated");
    const s = bytes.slice(at, at + k);
    at += k;
    return s;
  };
  const entries: ZSigEntry[] = [];
  for (let i = 0; i < n; i++) {
    const h = take(5);
    const pool = h[0]!;
    const index = (h[1]! | (h[2]! << 8) | (h[3]! << 16) | (h[4]! << 24)) >>> 0;
    if (pool === ZPool.Transparent) {
      if (version === 1) fail("a transparent entry in a version 1 file");
      const l = take(1)[0]!;
      if (l < TSIG_MIN || l > TSIG_MAX) fail(`transparent signature of ${l} bytes`);
      entries.push({ pool, index, sig: take(l), pubkey: take(33) });
    } else if (pool === ZPool.Orchard || pool === ZPool.Ironwood) {
      entries.push({ pool, index, sig: take(64) });
    } else {
      fail(`unknown pool ${pool}`);
    }
  }
  if (at !== bytes.length) fail(`${bytes.length - at} trailing bytes`);
  return { version, pcztSha256, entries };
}

/** `send.pczt` -> `send.zsig`; anything else gets `.zsig` appended. */
export function zsigName(pcztName: string): string {
  const base = pcztName.replace(/^.*[\\/]/, "").replace(/\.pczt$/i, "");
  return `${base || "signed"}.zsig`;
}

/**
 * A board error as a person reads it: the board's own message verbatim,
 * with the code, so a refusal added to the firmware later (a network guard,
 * say) reads as the board wrote it rather than as a guess made here.
 */
export function zErrorText(e: unknown): string {
  if (e instanceof DeviceError) {
    if ((e.code & 0xff00) === ZErr.SignBase && !/refused to sign/.test(e.message)) {
      return `The board refused: ${e.message}`;
    }
    return e.message;
  }
  return e instanceof Error ? e.message : String(e);
}
