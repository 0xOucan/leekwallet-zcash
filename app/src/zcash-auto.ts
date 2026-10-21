/**
 * The automatic wallet: what happens once the board is paired and unlocked,
 * with no clicks, the way Zodl's "connect the board" flow runs.
 *
 *   1. Read the board's account (zGetAccount), once more after 1.5 s on
 *      0x0300 while its vault is still opening.
 *   2. Its network, seed fingerprint and account name the wallet on this
 *      device: open it if it exists.
 *   3. Otherwise create it from the board's viewing key, born at the
 *      board's birthday for that network; a board without one gets the
 *      chain tip minus BIRTHDAY_MARGIN, recorded on the board
 *      (zSetAccountMeta) for the next companion, as the desktop app does.
 *   4. The caller then starts syncing (src/sync-scheduler.ts).
 *
 * No DOM here, so the whole flow runs under Node against zdev and a
 * scripted wallet (test/zcash-auto.test.ts). The manual buttons (Board ->
 * Read the board's account, Wallet -> Import) stay for troubleshooting.
 */

import { zGetAccount, zSetBirthday, type ZAccount, type ZCall, type ZNetwork } from "../packages/core/src/zcash.ts";
import type { Invoke } from "./tauri-transport.ts";

/**
 * Blocks below the tip a new wallet is born at, when the board has no
 * birthday: about 2 hours of blocks, so a payment made while the wallet was
 * being set up is still scanned.
 */
export const BIRTHDAY_MARGIN = 100;

/** How long to wait before asking again after 0x0300. */
export const RETRY_0300_MS = 1500;

/**
 * zGetAccount, retried once on 0x0300: right after the PIN the board can
 * answer "no wallet on the device" or "derivation failed" while its vault
 * is still opening, and a read a moment later works (docs/JOURNAL.md).
 */
export async function readAccountRetrying(
  call: ZCall, account: number, network: ZNetwork,
  { retryMs = RETRY_0300_MS, onRetry }: { retryMs?: number; onRetry?: () => void } = {},
): Promise<ZAccount> {
  try {
    return await zGetAccount(call, account, network);
  } catch (e) {
    if ((e as { code?: unknown }).code !== 0x0300) throw e;
    onRetry?.();
    await new Promise((r) => setTimeout(r, retryMs));
    return zGetAccount(call, account, network);
  }
}

/** What the wallet commands report about a wallet (zw_status, the part used here). */
interface WalletStatus { hasAccount: boolean; birthday: number | null }

export interface EnsureDeps {
  call: ZCall;
  invoke: Invoke;
  /** One line of progress for the status bar. */
  say?: (text: string) => void;
  log?: (line: string) => void;
}

export interface Ensured {
  /** True when this run created the wallet, false when it existed. */
  created: boolean;
  /** The wallet's birthday, when known. */
  birthday: number | null;
  /** Where the birthday came from, for the status line. */
  birthdaySource: "existing" | "board" | "tip";
  /** True when the birthday was written to the board by this run. */
  storedOnBoard: boolean;
}

/**
 * Open the wallet of `a` (the board's account just read), creating it when
 * this device has none for its network, seed fingerprint and account.
 */
export async function ensureWallet(a: ZAccount, d: EnsureDeps): Promise<Ensured> {
  const key = { network: a.network, wallet: a.seedFingerprint, account: a.account };
  const s = await d.invoke<WalletStatus>("zw_status", key);
  if (s.hasAccount) return { created: false, birthday: s.birthday, birthdaySource: "existing", storedOnBoard: false };

  let birthday = a.birthday;
  let source: Ensured["birthdaySource"] = "board";
  let storedOnBoard = false;
  if (birthday === null) {
    d.say?.("The board has no birthday for this network: asking the server for the chain tip…");
    const tip = await d.invoke<{ height: number }>("zw_tip", { network: a.network });
    birthday = Math.max(1, tip.height - BIRTHDAY_MARGIN);
    source = "tip";
  }
  d.say?.(`Creating the wallet of account ${a.account}, born at block ${birthday}…`);
  const r = await d.invoke<{ alreadyImported: boolean; birthday: number }>("zw_import", {
    network: a.network, ufvk: a.ufvk, seedFingerprint: a.seedFingerprint, account: a.account, ua: a.ua, birthday,
  });
  /* Recorded on the board, so the next companion (or this one after a
   * reinstall) starts from the same block without asking. */
  if (a.birthday !== r.birthday) {
    d.say?.(`Recording birthday ${r.birthday} on the board…`);
    try {
      await zSetBirthday(d.call, a.account, a.network, r.birthday);
      storedOnBoard = true;
    } catch (e) {
      /* The wallet exists and syncs either way; only the next companion
       * would have to be told the birthday. */
      d.log?.(`zcash: birthday ${r.birthday} not recorded on the board: ${(e as Error).message}`);
    }
  }
  d.log?.(`zcash: created the ${a.network} wallet of account ${a.account} (birthday ${r.birthday}, from the ${source})`);
  return { created: !r.alreadyImported, birthday: r.birthday, birthdaySource: source, storedOnBoard };
}
