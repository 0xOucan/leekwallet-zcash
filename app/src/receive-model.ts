/**
 * What the Receive screen shows, without the DOM (tested under Node:
 * test/receive-model.test.ts).
 *
 * Two views of the shielded address, switched by a toggle:
 *
 * - **Fresh** (the default, as in Zodl): a new diversified unified address
 *   each time Receive opens or "New address" is pressed. Every one of them
 *   reaches this wallet, and payers cannot link them to each other.
 * - **Board address (index 0)**: the address the board itself shows on its
 *   own Receive screen (zGetAccount's `ua`), so the two can be compared by
 *   eye.
 *
 * Either one is checked on the board with zShowAddress, which takes the
 * address shown here: the board derives its own receiver at that
 * diversifier index and refuses an address that is not its own.
 *
 * Transparent: the account's default address (index 0, the board's
 * tAddress), or, on "New transparent address", the transparent receiver of
 * the next diversified unified address. The board checks a transparent
 * receiver through that unified address (it compares the P2PKH receiver
 * with its key at the same index), so the model keeps the pair.
 */

export interface NewAddress { unified: string; transparent: string | null }
export type ReceiveMode = "fresh" | "board";

export interface ShieldedView {
  title: string;
  ua: string | null;
  /** What to send to zShowAddress; null when there is nothing to check. */
  verifyUa: string | null;
  note: string;
}

export interface TransparentView {
  title: string;
  address: string | null;
  verifyUa: string | null;
  note: string;
}

export class ReceiveModel {
  private readonly rotateFn: () => Promise<NewAddress>;
  mode: ReceiveMode = "fresh";
  /** The last address handed out by rotation. */
  fresh: NewAddress | null = null;
  /** The wallet's default addresses (index 0). */
  defaults: { unified: string; transparent: string | null } | null = null;
  /** The board's own account (zGetAccount): what its Receive screen shows. */
  board: { ua: string; tAddress: string } | null = null;
  /** A transparent receiver handed out by "New transparent address", with its UA. */
  freshT: { address: string; ua: string } | null = null;
  /** Why the last rotation failed, if it did. */
  lastError: string | null = null;

  constructor(rotate: () => Promise<NewAddress>) {
    this.rotateFn = rotate;
  }

  /** Another wallet (or none): forget everything. */
  reset(): void {
    this.fresh = null;
    this.defaults = null;
    this.board = null;
    this.freshT = null;
    this.lastError = null;
    this.mode = "fresh";
  }

  /**
   * A new diversified address (Receive opened, or "New address"). On
   * failure (the wallet busy syncing, say) the previous one stays: it is
   * still this wallet's, only not new.
   */
  async rotate(): Promise<boolean> {
    try {
      this.fresh = await this.rotateFn();
      this.lastError = null;
      return true;
    } catch (e) {
      this.lastError = (e as Error).message ?? String(e);
      return false;
    }
  }

  /** "New transparent address": the transparent receiver of the next diversified address. */
  async newTransparent(): Promise<string | null> {
    try {
      const a = await this.rotateFn();
      if (!a.transparent) {
        this.lastError = "The wallet handed out a shielded-only address (too many unused transparent addresses): use one already shown.";
        return null;
      }
      this.freshT = { address: a.transparent, ua: a.unified };
      this.fresh = a;
      this.lastError = null;
      return a.transparent;
    } catch (e) {
      this.lastError = (e as Error).message ?? String(e);
      return null;
    }
  }

  shielded(): ShieldedView {
    if (this.mode === "board") {
      return {
        title: "Board address (index 0)",
        ua: this.board?.ua ?? null,
        verifyUa: this.board?.ua ?? null,
        note: "The same address the board shows on its own Receive screen. It never changes, so payers who reuse it can be linked.",
      };
    }
    const ua = this.fresh?.unified ?? this.defaults?.unified ?? null;
    return {
      title: "Shielded address (new each time)",
      ua,
      verifyUa: ua,
      note: this.fresh
        ? "A new address for this payer; every earlier one still reaches this wallet."
        : "The wallet's default address (a new one could not be made yet).",
    };
  }

  transparent(): TransparentView {
    if (this.mode !== "board" && this.freshT) {
      return {
        title: "Transparent address (new)",
        address: this.freshT.address,
        verifyUa: this.freshT.ua,
        note: "Public on the chain, like Bitcoin. The board checks it through the unified address that carries it. Shield what arrives.",
      };
    }
    const address = this.board?.tAddress ?? this.defaults?.transparent ?? null;
    return {
      title: "Transparent address (index 0)",
      address,
      verifyUa: this.board?.ua ?? this.defaults?.unified ?? null,
      note: "Public on the chain: anyone can see what it receives. Shield what arrives.",
    };
  }
}
