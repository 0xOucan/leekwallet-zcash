/**
 * Tracking device state on the host, and knowing when to forget it.
 *
 * The device clears the passphrase whenever it locks, switches wallet, or
 * disconnects. Anything the host derived under that passphrase is then stale,
 * and stale is not a cosmetic problem here: the addresses on screen belong to a
 * wallet the device can no longer produce, and the user has no way to tell by
 * looking.
 *
 * So the rule is narrow and absolute: **derived state is only valid for the
 * exact (unlocked, wallet, passphrase, account) tuple it was derived under.**
 * Any change invalidates it, including a change the user made on the device
 * without telling the app.
 *
 * `account` joined that tuple late. The device has always had an account
 * selector — a separate identity off the same seed — but it was invisible to
 * the host, so turning it on the device left the app listing the old account's
 * addresses with nothing saying so. Signing was never at risk (the device
 * renders the whole path on its confirmation screens), but a receive address
 * copied from the app while the device browsed elsewhere is somebody watching
 * the wrong balance.
 */

import type { CborValue } from "./cbor.ts";
import type { ZNetwork } from "./zcash.ts";

export interface DeviceStatus {
  unlocked: boolean;
  walletCount: number;
  activeWallet: number;
  /**
   * Whether the device is signing from a temporary seed — one held in RAM and
   * stored nowhere. `activeWallet` is 0 for the whole of it, because no stored
   * seed is selected, so without this the app cannot tell that mode from a
   * fault and renders `wallet 0/N`.
   */
  temporary: boolean;
  /** Whether a passphrase is applied. Never *which* one — see docs/VAULT.md. */
  passphrase: boolean;
  /**
   * The BIP44 account the device's own screens are browsing.
   *
   * Not a secret and not a security boundary: every account comes off the same
   * master key, so anyone with the seed can derive all of them. It separates
   * identities, it does not hide them.
   */
  account: number;
  /**
   * The network the board is set to under Settings > Network. The board
   * refuses z* requests for the other one (0x0528), so a companion reads this
   * to say so, or to follow the board, before it asks. Null on firmware that
   * predates the field.
   *
   * Not part of the invalidation tuple: switching it derives nothing new for
   * the network the app is on, it only makes the board refuse it.
   */
  network: ZNetwork | null;
}

/** A getStatus result, as the board writes it (protocol.c). */
export function parseStatus(s: Record<string, CborValue>): DeviceStatus {
  const net = s["network"];
  return {
    unlocked: s["unlocked"] === 1,
    walletCount: Number(s["walletCount"] ?? 0),
    activeWallet: Number(s["activeWallet"] ?? 0),
    /* Absent on firmware that predates the field, where it is also always
     * false: temporary mode and this flag shipped together. */
    temporary: s["temporary"] === 1,
    passphrase: s["passphrase"] === 1,
    /* Absent on firmware older than this field. Defaulting to 0 makes such a
     * device look permanently parked on the default account, which is what it
     * effectively is from the host's side: it never reports a change, so
     * nothing is ever invalidated for a reason the app cannot see. */
    account: Number(s["account"] ?? 0),
    network: net === "main" || net === "test" ? net : null,
  };
}

export const UNKNOWN_STATUS: DeviceStatus = {
  unlocked: false,
  walletCount: 0,
  activeWallet: 0,
  temporary: false,
  passphrase: false,
  account: 0,
  network: null,
};

/**
 * Does moving from `before` to `after` invalidate anything derived earlier?
 *
 * Deliberately conservative: it answers yes on any transition that could change
 * a derivation, including locking. Being wrong in the other direction means
 * showing someone an address that is not theirs.
 */
export function derivationsInvalidated(before: DeviceStatus, after: DeviceStatus): boolean {
  if (before.unlocked && !after.unlocked) return true;      // locked
  if (!before.unlocked && after.unlocked) return true;      // fresh unlock, passphrase may differ
  if (before.activeWallet !== after.activeWallet) return true;
  /* Nearly always redundant, because loading a temporary seed moves
   * activeWallet to 0 and every way back out moves it off 0 again. The
   * exception is a device with a PIN and no stored wallets: activeWallet is
   * already 0, so entering temporary mode changes no other field, and the app
   * would go on believing there was nothing to derive. Comparing the flag
   * makes the rule complete rather than nearly complete -- the tuple this
   * function claims to watch is the tuple it should watch. */
  if (before.temporary !== after.temporary) return true;
  if (before.passphrase !== after.passphrase) return true;
  if (before.account !== after.account) return true;
  return false;
}

/**
 * Never persist derived addresses.
 *
 * A passphrase wallet leaves no trace on the device by design, and writing its
 * addresses into host storage undoes exactly that: anyone reading the app's
 * data learns a hidden wallet exists, which is the fact the passphrase was
 * protecting. Keep them in memory, drop them on invalidation, and never write
 * them anywhere.
 */
export const PERSIST_DERIVED_ADDRESSES = false;
