/**
 * Import from the board's QR: a wallet set up on a companion
 * that has never reached the board over USB or Bluetooth.
 *
 *   1. On the board: Main menu → View key, confirmed there. It shows its
 *      account as an animated ur:leek-zcash-accounts (the AccountRecord of
 *      host/leek-zcash/src/record.rs: seed fingerprint, UFVK, account index).
 *   2. The camera (src/qr-scan.ts, as for the Sign with QR reply) feeds
 *      every code to the backend's decoder (src-tauri/src/qr_accounts.rs),
 *      which checks the network and derives the account's default address
 *      from the viewing key itself.
 *   3. With a birthday (typed, or the chain tip minus BIRTHDAY_MARGIN, as
 *      the automatic set-up does for a board that has none), zw_import
 *      makes it a normal board wallet: signable over QR, USB or Bluetooth.
 *
 * One way only: a viewing key comes in, nothing goes to the board. This is
 * never pairing; USB and Bluetooth keep their 6-digit pairing code.
 *
 * No DOM here: the state machine runs under Node (test/qr-sign.test.ts).
 */

import type { Invoke } from "./tauri-transport.ts";
import { BIRTHDAY_MARGIN } from "./zcash-auto.ts";
import type { ZNetwork } from "../packages/core/src/zcash.ts";

/** One account of the record (qr_accounts.rs QrAccount). */
export interface QrAccount { ufvk: string; account: number; ua: string; name: string | null; birthday: number | null }
/** The record (qr_accounts.rs QrAccountRecord). */
export interface QrAccountRecord { network: ZNetwork; seedFingerprint: string; zfp: string; accounts: QrAccount[] }
export interface AccountsProgress {
  state: "accepted" | "redundant" | "ignored" | "rejected" | "refused" | "complete";
  received: number;
  total: number;
  error: string | null;
  record: QrAccountRecord | null;
}

export type ImportPhase = "idle" | "scanning" | "found" | "importing" | "done" | "failed";

export class QrAccountImport {
  phase: ImportPhase = "idle";
  received = 0;
  total = 0;
  /** Why the last code was not taken (scanning goes on). */
  note = "";
  error: string | null = null;
  record: QrAccountRecord | null = null;
  network: ZNetwork = "test";
  onChange: () => void = () => undefined;
  private feeding: Promise<void> = Promise.resolve();
  private generation = 0;

  private readonly invoke: Invoke;

  constructor(invoke: Invoke) {
    this.invoke = invoke;
  }

  async start(network: ZNetwork): Promise<void> {
    this.generation++;
    this.network = network;
    this.received = 0;
    this.total = 0;
    this.note = "";
    this.error = null;
    this.record = null;
    await this.invoke<void>("qr_accounts_reset");
    this.phase = "scanning";
    this.onChange();
  }

  /** One decoded QR text. Resolves true once the scan should stop (found or refused). */
  feed(text: string): Promise<boolean> {
    const gen = this.generation;
    const run = this.feeding.then(async (): Promise<boolean> => {
      if (this.phase !== "scanning" || gen !== this.generation) return false;
      const p = await this.invoke<AccountsProgress>("qr_accounts_feed", { network: this.network, text });
      if (this.phase !== "scanning" || gen !== this.generation) return false;
      if (p.state === "ignored") return false;
      this.received = p.received;
      this.total = p.total;
      if (p.state === "rejected") this.note = p.error ?? "a QR code that is not the board's view key";
      else if (p.state === "accepted") this.note = "";
      if (p.state === "refused") {
        this.fail(new Error(p.error ?? "The board's account cannot be imported here."));
        return true;
      }
      if (p.state === "complete" && p.record) {
        if (p.record.accounts.length === 0) {
          this.fail(new Error("The board's code holds no account."));
          return true;
        }
        this.record = p.record;
        this.phase = "found";
        this.onChange();
        return true;
      }
      this.onChange();
      return false;
    });
    this.feeding = run.then(() => undefined, () => undefined);
    return run;
  }

  /** The account to import (the board sends one). */
  account(): QrAccount | null {
    return this.record?.accounts[0] ?? null;
  }

  /**
   * zw_import the account found, born at `typed` (a block height) or, when
   * empty, at the record's own birthday, else the chain tip minus
   * BIRTHDAY_MARGIN: the board set-up's rule for a board without one.
   */
  async importFound(typed: string): Promise<{ alreadyImported: boolean; birthday: number; key: { network: ZNetwork; wallet: string; account: number } }> {
    const r = this.record;
    const a = this.account();
    if (this.phase !== "found" || !r || !a) throw new Error("Scan the board's view key first.");
    const birthday = await resolveBirthday(typed, a.birthday, () => this.invoke<{ height: number }>("zw_tip", { network: r.network }));
    this.phase = "importing";
    this.onChange();
    try {
      const out = await this.invoke<{ alreadyImported: boolean; birthday: number }>("zw_import", {
        network: r.network, ufvk: a.ufvk, seedFingerprint: r.seedFingerprint, account: a.account, ua: a.ua, birthday,
      });
      this.phase = "done";
      this.onChange();
      return { ...out, key: { network: r.network, wallet: r.seedFingerprint, account: a.account } };
    } catch (e) {
      /* Still found: fix the birthday and import again. */
      this.phase = "found";
      this.error = errorText(e);
      this.onChange();
      throw e;
    }
  }

  cancel(): void {
    this.generation++;
    this.phase = "idle";
    this.record = null;
    this.onChange();
  }

  fail(e: unknown): void {
    this.generation++;
    this.phase = "failed";
    this.error = errorText(e);
    this.onChange();
  }
}

function errorText(e: unknown): string {
  return (e as Error)?.message ?? String(e);
}

/** A typed height, the record's, or the tip minus BIRTHDAY_MARGIN. */
export async function resolveBirthday(typed: string, recorded: number | null, tip: () => Promise<{ height: number }>): Promise<number> {
  const t = typed.trim();
  if (t) {
    const n = Number(t);
    if (!/^\d+$/.test(t) || !Number.isSafeInteger(n) || n < 1) {
      throw new Error("Enter the birthday as a block height (digits only), or leave it empty for a new seed.");
    }
    return n;
  }
  if (recorded !== null) return recorded;
  return Math.max(1, (await tip()).height - BIRTHDAY_MARGIN);
}

/** The scan's progress line: "N of M parts". */
export function accountsCaption(q: Pick<QrAccountImport, "received" | "total" | "note">): string {
  const base = q.total === 0
    ? "Point the camera at the board's view key QR (Main menu → View key)…"
    : `${q.received} of ${q.total} part${q.total === 1 ? "" : "s"} read…`;
  return q.note ? `${base} (ignored one code: ${q.note})` : base;
}

/** What was found, as label/value rows. */
export function recordRows(r: QrAccountRecord, networkLabel: (n: ZNetwork) => string): [string, string][] {
  const a = r.accounts[0];
  const rows: [string, string][] = [
    ["Network", networkLabel(r.network)],
    ["Account", a ? String(a.account) : "?"],
    ["Seed fingerprint", `ZFP ${r.zfp}`],
  ];
  if (a) rows.push(["Address", `${a.ua.slice(0, 16)}…${a.ua.slice(-8)}`]);
  if (r.accounts.length > 1) rows.push(["Note", `the code holds ${r.accounts.length} accounts; account ${a!.account} is imported`]);
  return rows;
}
