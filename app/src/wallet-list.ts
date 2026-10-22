/**
 * The wallets on this device (index.html #walletspanel), from the launcher:
 * every wallet of both networks, to open without the board, and a viewing
 * key to import by hand (watch-only), as the old desktop app allowed.
 *
 * The list is zw_wallets (src-tauri/src/zwallet.rs over app/zcash-wallet's
 * keys.rs), which also settles a wallet of the old one-per-network layout
 * (the first Companion builds, or one copied from the old desktop app on
 * the first start) under the account it holds. Opening one hands its key
 * to the wallet screens (src/zcash-wallet.ts, openWithoutBoard).
 */

import { zErrorText, type ZNetwork } from "../packages/core/src/zcash.ts";
import type { Invoke } from "./tauri-transport.ts";
import { networkLabel } from "./zcash-view.ts";
import type { Key } from "./zcash-wallet.ts";

/** A wallet, as zw_wallets lists it. */
export interface WalletInfo {
  network: ZNetwork;
  wallet: string;
  account: number;
  watchOnly: boolean;
  zfp: string | null;
  birthday: number | null;
  fullyScanned: number | null;
}

/** "Board ZFP 551A1FD4, account 0" or "Viewing key 0011…", for the list. */
export function walletLabel(w: WalletInfo): string {
  return w.watchOnly
    ? `Viewing key ${w.wallet.slice("watch-".length, "watch-".length + 8)} (watch-only)`
    : `Board ZFP ${w.zfp ?? w.wallet.slice(0, 8)}, account ${w.account}`;
}

/** What else the list says about a wallet: its birthday and last scan. */
export function walletDetail(w: WalletInfo): string {
  const born = w.birthday === null ? "" : `born at block ${w.birthday}`;
  const scanned = w.fullyScanned === null ? "never synced" : `synced to ${w.fullyScanned}`;
  return [born, scanned].filter((x) => x).join(" · ");
}

export interface WalletListDeps {
  invoke: Invoke;
  /** Open `key` without the board. */
  open(key: Key): void;
  log(line: string): void;
}

export interface WalletList {
  /** The panel is shown: read the wallets again. */
  refresh(): Promise<void>;
}

const $ = <T extends HTMLElement>(id: string): T => {
  const el = document.getElementById(id);
  if (!el) throw new Error(`missing element #${id}`);
  return el as T;
};

export function initWalletList(deps: WalletListDeps): WalletList {
  const say = (id: string, t: string) => { $(id).textContent = t; };

  async function refresh(): Promise<void> {
    say("wliststatus", "Reading the wallets on this device…");
    let all: WalletInfo[];
    try {
      const [t, m] = await Promise.all(
        (["test", "main"] as ZNetwork[]).map((network) => deps.invoke<WalletInfo[]>("zw_wallets", { network })),
      );
      all = [...m!, ...t!];
    } catch (e) {
      say("wliststatus", zErrorText(e));
      return;
    }
    const list = $("wlist");
    list.replaceChildren(...all.map((w) => {
      const li = document.createElement("li");
      const head = document.createElement("div");
      head.className = "zhist__head";
      const net = document.createElement("span");
      net.className = "badge";
      net.dataset["net"] = w.network;
      net.textContent = networkLabel(w.network);
      head.append(document.createTextNode(walletLabel(w)), net);
      const detail = document.createElement("div");
      detail.className = "muted";
      detail.textContent = walletDetail(w);
      const open = document.createElement("button");
      open.type = "button";
      open.className = "secondary";
      open.textContent = "Open";
      open.addEventListener("click", () => deps.open({ network: w.network, wallet: w.wallet, account: w.account }));
      li.append(head, detail, open);
      return li;
    }));
    say("wliststatus", all.length === 0
      ? "No wallets on this device yet. Connect the board (it sets its wallet up by itself), or import a viewing key below."
      : "");
  }

  $("wtip").addEventListener("click", () => {
    const network = $<HTMLSelectElement>("wnet").value as ZNetwork;
    void deps.invoke<{ height: number }>("zw_tip", { network }).then(
      (t) => { $<HTMLInputElement>("wbday").value = String(t.height); },
      (e) => say("wimportstatus", zErrorText(e)),
    );
  });

  $("wimport").addEventListener("click", () => {
    const network = $<HTMLSelectElement>("wnet").value as ZNetwork;
    const ufvk = $<HTMLTextAreaElement>("wufvk").value.trim();
    const birthday = Number($<HTMLInputElement>("wbday").value.trim());
    if (!ufvk) { say("wimportstatus", "Paste the viewing key."); return; }
    if (!Number.isInteger(birthday) || birthday < 1) {
      say("wimportstatus", "Enter the birthday: the height of the first block that can hold the account's funds (for a new account, today's tip).");
      return;
    }
    const btn = $<HTMLButtonElement>("wimport");
    btn.disabled = true;
    say("wimportstatus", `Importing the viewing key on ${networkLabel(network)}, born at block ${birthday}…`);
    void deps.invoke<{ wallet: string; alreadyImported: boolean }>("zw_import_watch", { network, ufvk, birthday }).then((r) => {
      $<HTMLTextAreaElement>("wufvk").value = "";
      say("wimportstatus", r.alreadyImported ? "This viewing key was already here." : "Imported. Opening it…");
      deps.log(`zcash: imported a ${network} viewing key (watch-only)`);
      deps.open({ network, wallet: r.wallet, account: 0 });
    }, (e) => say("wimportstatus", zErrorText(e))).finally(() => { btn.disabled = false; });
  });

  return { refresh };
}
