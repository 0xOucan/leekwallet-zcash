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
 *
 * And the board's account read off its own screen (EXPERIMENTAL, Main menu
 * → View key; src/qr-accounts.ts): a normal board wallet for a board kept
 * off USB and Bluetooth, a one-way import that never pairs.
 */

import { zErrorText, type ZNetwork } from "../packages/core/src/zcash.ts";
import type { Invoke } from "./tauri-transport.ts";
import { networkLabel } from "./zcash-view.ts";
import type { Key } from "./zcash-wallet.ts";
import { qrScanningAvailable, scanQr, type QrScan } from "./qr-scan.ts";
import { accountsCaption, recordRows, QrAccountImport } from "./qr-accounts.ts";

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

  /* ------------------------------------- import from the board's QR */

  const qr = new QrAccountImport(deps.invoke);
  let camera: QrScan | null = null;

  const stopCamera = (): void => {
    camera?.stop();
    camera = null;
    $("wqrscanbox").hidden = true;
  };

  const renderQr = (): void => {
    const scanning = qr.phase === "scanning";
    $<HTMLButtonElement>("wqrscan").disabled = scanning || qr.phase === "importing";
    $<HTMLSelectElement>("wqrnet").disabled = scanning || qr.phase === "found" || qr.phase === "importing";
    const bar = $<HTMLProgressElement>("wqrbar");
    bar.max = Math.max(1, qr.total);
    bar.value = qr.received;
    const r = qr.record;
    $("wqrfound").hidden = !(r && (qr.phase === "found" || qr.phase === "importing"));
    $<HTMLButtonElement>("wqrimport").disabled = qr.phase === "importing";
    if (r) {
      const kv = $("wqrinfo");
      kv.replaceChildren(...recordRows(r, networkLabel).flatMap(([k, v]) => {
        const dt = document.createElement("dt");
        dt.textContent = k;
        const dd = document.createElement("dd");
        dd.textContent = v;
        return [dt, dd];
      }));
    }
    if (scanning) say("wqrstatus", accountsCaption(qr));
    else if (qr.phase === "found") say("wqrstatus", qr.error ?? "Read. Check the seed fingerprint against the board (ZFP), choose the birthday, then Import.");
    else if (qr.phase === "failed") say("wqrstatus", qr.error ?? "The board's QR could not be read.");
  };
  qr.onChange = renderQr;

  $("wqrscan").addEventListener("click", () => {
    if (!qrScanningAvailable()) {
      say("wqrstatus", "This window cannot reach a camera. Connect the board over USB or Bluetooth instead.");
      return;
    }
    stopCamera();
    const network = $<HTMLSelectElement>("wqrnet").value as ZNetwork;
    void (async () => {
      await qr.start(network);
      $("wqrscanbox").hidden = false;
      try {
        camera = await scanQr<never>(
          $<HTMLVideoElement>("wqrvideo"),
          /* Every code goes to the decoder; the camera stops from here. */
          (t) => {
            void qr.feed(t).then((stop) => { if (stop) stopCamera(); }, (e) => { stopCamera(); qr.fail(e); });
            return undefined;
          },
          () => undefined,
          (m) => { stopCamera(); qr.fail(new Error(m)); },
          { animated: true },
        );
        /* Read whole before the camera was even up. */
        if (qr.phase !== "scanning") stopCamera();
      } catch (e) {
        stopCamera();
        const name = (e as { name?: string }).name;
        qr.fail(new Error(name === "NotAllowedError"
          ? "The camera was not allowed. Allow it for Zcash LeekWallet in the system settings."
          : name === "NotFoundError" ? "No camera found." : `The camera did not start: ${(e as Error).message ?? String(e)}`));
      }
    })().catch((e) => { stopCamera(); qr.fail(e); });
  });

  /* No camera left running behind another screen. */
  const leave = () => { if (camera) { stopCamera(); qr.cancel(); say("wqrstatus", ""); } };
  $("walletsback").addEventListener("click", leave);
  document.addEventListener("visibilitychange", () => { if (document.hidden) leave(); });

  $("wqrstop").addEventListener("click", () => {
    stopCamera();
    qr.cancel();
    say("wqrstatus", "Stopped. Nothing was imported.");
  });

  $("wqrdiscard").addEventListener("click", () => {
    qr.cancel();
    $<HTMLInputElement>("wqrbday").value = "";
    say("wqrstatus", "Discarded. Nothing was imported.");
  });

  $("wqrtip").addEventListener("click", () => {
    const network = qr.record?.network ?? ($<HTMLSelectElement>("wqrnet").value as ZNetwork);
    void deps.invoke<{ height: number }>("zw_tip", { network }).then(
      (t) => { $<HTMLInputElement>("wqrbday").value = String(t.height); },
      (e) => say("wqrstatus", zErrorText(e)),
    );
  });

  $("wqrimport").addEventListener("click", () => {
    const typed = $<HTMLInputElement>("wqrbday").value;
    const a = qr.account();
    if (!a) return;
    say("wqrstatus", typed.trim()
      ? `Importing account ${a.account}, born at block ${typed.trim()}…`
      : `Importing account ${a.account}: asking the server for the chain tip for its birthday…`);
    void qr.importFound(typed).then((r) => {
      $<HTMLInputElement>("wqrbday").value = "";
      say("wqrstatus", r.alreadyImported
        ? `This account was already on this device (born at block ${r.birthday}). Opening it…`
        : `Imported, born at block ${r.birthday}. Opening it…`);
      deps.log(`zcash: imported account ${r.key.account} of ZFP ${r.key.wallet.slice(0, 8).toUpperCase()} into the ${r.key.network} wallet from the board's QR`);
      deps.open(r.key);
      void refresh();
    }, (e) => say("wqrstatus", zErrorText(e)));
  });

  return { refresh };
}
