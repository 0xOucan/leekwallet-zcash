/**
 * The wallet commands of src-tauri/src/zwallet.rs, in memory, for the
 * automatic set-up tests: zw_status, zw_tip and zw_import, keyed by
 * network, seed fingerprint and account as the real ones are. Records
 * every command it was asked.
 */

import type { Invoke } from "../src/tauri-transport.ts";

export interface FakeWallet {
  invoke: Invoke;
  calls: string[];
  /** network-fingerprint-account -> birthday */
  wallets: Map<string, number>;
}

export function fakeWallet({ tip }: { tip: number }): FakeWallet {
  const wallets = new Map<string, number>();
  const calls: string[] = [];
  const id = (a: Record<string, unknown>) => `${String(a["network"])}-${String(a["wallet"] ?? a["seedFingerprint"])}-${String(a["account"])}`;
  const invoke = (async (cmd: string, args: Record<string, unknown> = {}) => {
    calls.push(cmd);
    switch (cmd) {
      case "zw_status": {
        const b = wallets.get(id(args));
        return { hasAccount: b !== undefined, birthday: b ?? null };
      }
      case "zw_tip":
        return { height: tip };
      case "zw_import": {
        const k = id(args);
        const had = wallets.get(k);
        if (had !== undefined) return { alreadyImported: true, birthday: had };
        const b = Number(args["birthday"]);
        if (!Number.isInteger(b) || b < 1) throw new Error("bad birthday");
        if (typeof args["ufvk"] !== "string" || typeof args["ua"] !== "string") throw new Error("no key");
        wallets.set(k, b);
        return { alreadyImported: false, birthday: b };
      }
      default:
        throw new Error(`fake wallet: no ${cmd}`);
    }
  }) as Invoke;
  return { invoke, calls, wallets };
}
