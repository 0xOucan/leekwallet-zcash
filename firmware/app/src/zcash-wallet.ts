/**
 * The Zcash wallet screens (index.html #zpane-wallet and its siblings):
 * home with the balance, sync and the shield prompt; receive with a fresh
 * address each time and the board's own; send with a QR scanner, contacts,
 * memo and review; history; the address book; server and wallet settings.
 *
 * The screens follow Zodl's (the Zashi-derived Android wallet in android/,
 * MIT) as a feature reference; nothing of its code is used. The engine is
 * zecz (host/zecz-core) in src-tauri/src/zwallet.rs. The board is reached
 * the way the rest of the Companion reaches it, through the Client, so a
 * send is: propose there, sign here on the board, finish there.
 *
 * Automatic, like Zodl's board flow: once the board is paired and unlocked,
 * its account is read (zcash-view.ts), the wallet of its network, seed
 * fingerprint and account is opened or created at the board's birthday
 * (zcash-auto.ts), and it syncs on open, every 75 s in the foreground and
 * after a send (sync-scheduler.ts). The manual import stays as the
 * fallback when the automatic set-up fails.
 *
 * One wallet per network, seed fingerprint and account. A BIP39 passphrase
 * on the board is another seed, another fingerprint, and so another wallet,
 * kept apart on disk (src-tauri/src/zwallet.rs). Every command names all
 * three, and an answer for a wallet the user has since left (another
 * network, account or seed) is dropped rather than drawn.
 */

import {
  encodeZsig, explorerTx, formatZec, memoBytes, MEMO_BYTES, toHex, zErrorText, zGetAccount,
  zSetBirthday, zShowAddress, zSign, type ZNetwork,
} from "../packages/core/src/zcash.ts";
import { addressKind, parsePaymentRequest } from "../packages/core/src/zip321.ts";
import type { Invoke } from "./tauri-transport.ts";
import { networkLabel, type ZcashDeps, type ZcashView } from "./zcash-view.ts";
import { ensureWallet } from "./zcash-auto.ts";
import { SyncScheduler } from "./sync-scheduler.ts";
import { ReceiveModel, type NewAddress } from "./receive-model.ts";
import { qrScanningAvailable, scanQr, type QrScan } from "./qr-scan.ts";

interface Pool { total: number; spendable: number; pending: number }
export interface Status {
  network: string; server: string; dataDir: string; hasAccount: boolean;
  birthday: number | null; fullyScanned: number | null; chainTip: number | null;
  hardware: boolean; zfp: string | null; defaultServer: string;
  seedFingerprint: string | null; accountIndex: number | null;
}
interface Balances {
  chainTip: number; fullyScanned: number; synced: boolean;
  orchard: Pool; ironwood: Pool; transparent: Pool; total: number; spendable: number;
}
interface Addresses { unified: string; transparent: string | null; orchardOnly: string }
interface Tx {
  txid: string; minedHeight: number | null; blockTime: number | null; expired: boolean;
  kind: "received" | "sent" | "shielded" | "internal"; amountZat: number; deltaZat: number;
  feeZat: number | null; memos: string[]; recipients: { address: string; valueZat: number }[];
}
interface Proposal {
  id: string; kind: "send" | "shield"; targetHeight: number;
  payments: { recipient: string; amountZat: number; pool: string; hasMemo: boolean }[];
  change: { valueZat: number; pool: string }[]; feeZat: number; totalZat: number;
  signerViewBytes: number; compact: boolean;
}
interface Contact { name: string; address: string }
type SyncEvent =
  | { network: string; kind: "started"; server: string; tip: number; from: number }
  | { network: string; kind: "scanned"; start: number; end: number; tip: number; fraction: number };
interface FinishEvent { network: string; step: string; message: string }

/** What the home screen says about syncing. */
type SyncState =
  | { kind: "idle" }
  | { kind: "syncing"; text: string; fraction: number | null }
  | { kind: "synced"; height: number; tip: number; at: Date }
  | { kind: "error"; text: string };

const HIDE_KEY = "leek.zcash.hide";
/** The wallet backend's refusal while another command runs on that network. */
const BUSY_RE = /wallet is busy/;

const $ = <T extends HTMLElement>(id: string): T => {
  const el = document.getElementById(id);
  if (!el) throw new Error(`missing element #${id}`);
  return el as T;
};

type Listen = <T>(event: string, cb: (e: { payload: T }) => void) => Promise<unknown>;
function listener(): Listen | null {
  const w = window as unknown as { __TAURI__?: { event?: { listen?: Listen } } };
  return w.__TAURI__?.event?.listen ?? null;
}

export interface WalletDeps extends ZcashDeps {
  open(url: string): void;
}

/** What the diagnostics report says about the wallet (main.ts). */
export interface WalletDiagnostics {
  /** The last zw_status of the wallet on screen, or null before one. */
  status: Status | null;
  /** When lightwalletd last answered (a sync, the tip, a server check). */
  lastContact: Date | null;
  /** How the last sync ended, and when. */
  lastSync: { at: Date; text: string } | null;
}

export interface ZcashWallet {
  diagnostics(): WalletDiagnostics;
}

export async function initZcashWallet(deps: WalletDeps, view: ZcashView): Promise<ZcashWallet | null> {
  const invoke0 = deps.invoke();
  if (!invoke0) return null;
  const invoke: Invoke = invoke0;
  try {
    await invoke<boolean>("zw_available");
  } catch {
    return null; // a build without the wallet: the board tools are all there is
  }
  for (const el of Array.from(document.querySelectorAll<HTMLElement>("#zcashpanel [data-wallet]"))) el.hidden = false;

  let status: Status | null = null;
  let addresses: Addresses | null = null;
  let proposal: { network: ZNetwork; key: { network: ZNetwork; wallet: string; account: number }; p: Proposal; view: Uint8Array } | null = null;
  let hide = false;
  try { hide = localStorage.getItem(HIDE_KEY) === "1"; } catch { /* shown */ }
  let lastContact: Date | null = null;
  let lastSync: { at: Date; text: string } | null = null;
  const contacted = () => { lastContact = new Date(); };

  const net = (): ZNetwork => view.network;
  const say = (id: string, text: string) => { $(id).textContent = text; };
  const zec = (zat: number) => (hide ? "•••• ZEC" : `${formatZec(zat)} ZEC`);

  /** Which wallet is on screen: the board account's seed fingerprint and
   * account on the current network, or none until the board was read. */
  interface Key { network: ZNetwork; wallet: string; account: number }
  const keyNow = (): Key | null => {
    const a = view.boardAccount;
    return a && a.network === net() ? { network: a.network, wallet: a.seedFingerprint, account: a.account } : null;
  };
  const sameKey = (a: Key | null, b: Key | null) =>
    a !== null && b !== null && a.network === b.network && a.wallet === b.wallet && a.account === b.account;
  const call = <T>(cmd: string, args: Record<string, unknown> = {}) => {
    const k = keyNow();
    if (!k) return Promise.reject(new Error("Reading the board's account first: its seed fingerprint names the wallet."));
    return invoke<T>(cmd, { ...k, ...args });
  };

  function selectTab(id: string): void {
    ($(id) as HTMLButtonElement).click();
  }
  const paneOpen = (id: string) => !$(id).hidden && !$("seczcash").hidden;

  function requireBoard() {
    const c = deps.call();
    if (!c) throw new Error("Connect and pair the board first (Link, above).");
    return c;
  }

  /**
   * The board connected now holds this wallet's seed, or a refusal. Signing
   * with another seed would at best fail on the board; checking first says
   * which board to fetch.
   */
  async function boardMatches(s: Status): Promise<number> {
    if (s.seedFingerprint === null || s.accountIndex === null) {
      throw new Error("This wallet has no board account; import it from the board first.");
    }
    const a = await zGetAccount(requireBoard(), s.accountIndex, net());
    if (a.seedFingerprint !== s.seedFingerprint) {
      throw new Error(`This board holds another seed than this wallet (ZFP ${s.zfp}). Connect the board that holds it, or open that seed on the board.`);
    }
    return s.accountIndex;
  }

  /* ------------------------------------------------------------- home */

  function renderPools(b: Balances | null): void {
    const pools = $("zwpools");
    pools.replaceChildren();
    if (!b) {
      say("zwtotal", "—");
      say("zwspendable", "Not synced yet.");
      $("zwshield").hidden = true;
      return;
    }
    say("zwtotal", zec(b.total));
    const waiting = b.orchard.pending + b.ironwood.pending + b.transparent.pending;
    say("zwspendable", `${zec(b.spendable)} spendable now`
      + (waiting ? ` · ${zec(waiting)} waiting for confirmations` : "")
      + (b.synced ? "" : " · still syncing: more may appear"));
    const row = (k: string, v: string) => {
      const dt = document.createElement("dt");
      dt.textContent = k;
      const dd = document.createElement("dd");
      dd.textContent = v;
      pools.append(dt, dd);
    };
    const pool = (p: Pool) => `${zec(p.total)}${p.pending ? ` (${zec(p.pending)} waiting)` : ""}`;
    const shielded: Pool = {
      total: b.orchard.total + b.ironwood.total,
      spendable: b.orchard.spendable + b.ironwood.spendable,
      pending: b.orchard.pending + b.ironwood.pending,
    };
    row("Shielded", pool(shielded));
    /* The two shielded pools only when both hold something: one line says
     * where the money is without a row of zeros. */
    if (b.orchard.total > 0 && b.ironwood.total > 0) {
      row("· Orchard", pool(b.orchard));
      row("· Ironwood", pool(b.ironwood));
    }
    row("Transparent", pool(b.transparent));
    /* Zodl's prompt: transparent funds are public, and shielding is one
     * board review away. Shown as soon as there are any; the button waits
     * until they are spendable. */
    const t = b.transparent;
    $("zwshield").hidden = t.total === 0;
    say("zwshieldamt", zec(t.total));
    $<HTMLButtonElement>("zwshieldgo").disabled = t.spendable === 0;
    $("zwshieldwait").hidden = t.spendable > 0;
  }

  function txRow(t: Tx): HTMLLIElement {
    const li = document.createElement("li");
    const head = document.createElement("div");
    head.className = "zhist__head";
    const what = { received: "Received", sent: "Sent", shielded: "Shielded", internal: "Moved" }[t.kind];
    const when = t.minedHeight === null
      ? (t.expired ? "expired, never mined" : "pending")
      : t.blockTime ? new Date(t.blockTime * 1000).toLocaleString() : `block ${t.minedHeight}`;
    const amt = document.createElement("span");
    amt.className = "zhist__amt";
    amt.dataset["dir"] = t.deltaZat < 0 ? "out" : "in";
    amt.textContent = hide ? "••••" : `${t.deltaZat < 0 ? "−" : "+"}${formatZec(Math.abs(t.deltaZat))} ZEC`;
    head.append(document.createTextNode(`${what} · ${when}`), amt);
    li.append(head);
    const details = document.createElement("details");
    const sum = document.createElement("summary");
    sum.textContent = t.memos.length ? `Memo: ${t.memos[0]!.slice(0, 60)}` : "Details";
    details.append(sum);
    const kv = document.createElement("dl");
    kv.className = "kv";
    const row = (k: string, v: string) => {
      const dt = document.createElement("dt");
      dt.textContent = k;
      const dd = document.createElement("dd");
      dd.textContent = v;
      kv.append(dt, dd);
    };
    row("Transaction", t.txid);
    if (t.feeZat !== null) row("Fee", hide ? "••••" : `${formatZec(t.feeZat)} ZEC`);
    for (const r of t.recipients) row("To", `${r.address} (${hide ? "••••" : formatZec(r.valueZat)})`);
    for (const m of t.memos) row("Memo", m);
    details.append(kv);
    const url = explorerTx(net(), t.txid);
    if (url) {
      const b = document.createElement("button");
      b.type = "button";
      b.className = "secondary";
      b.textContent = "Open in a block explorer";
      b.addEventListener("click", () => deps.open(url));
      details.append(b);
    }
    li.append(details);
    return li;
  }

  async function loadHistory(target: string, limit?: number): Promise<void> {
    const k = keyNow();
    const txs = await call<Tx[]>("zw_history");
    if (!sameKey(k, keyNow())) return;
    const list = $(target);
    list.replaceChildren(...(limit ? txs.slice(0, limit) : txs).map(txRow));
    if (txs.length === 0) {
      const li = document.createElement("li");
      li.className = "muted";
      li.textContent = "No transactions yet.";
      list.append(li);
    }
  }

  /** Nothing on screen of a wallet: no board account read yet (or it was
   * forgotten on lock, disconnect, another network or account). */
  function showNoWallet(): void {
    status = null;
    addresses = null;
    cancelProposal();
    for (const el of Array.from(document.querySelectorAll<HTMLElement>("#zcashpanel [data-account]"))) el.hidden = true;
    $("zwsetup").hidden = true;
    $("zwhome").hidden = true;
    renderPools(null);
    $("zwrecent").replaceChildren();
    $("zwhist").replaceChildren();
    contacts = [];
    renderContacts();
    $("zwinfo").replaceChildren();
    say("zwstatus", view.boardAccount === null
      ? "Reading the board's account: its seed fingerprint names the wallet on this device…"
      : "");
  }

  async function refresh(): Promise<void> {
    const k = keyNow();
    const n = net();
    if (!k) { showNoWallet(); return; }
    let s: Status;
    try {
      s = await call<Status>("zw_status");
    } catch (e) {
      say("zwstatus", zErrorText(e));
      return;
    }
    if (!sameKey(k, keyNow())) return;
    status = s;
    for (const el of Array.from(document.querySelectorAll<HTMLElement>("#zcashpanel [data-account]"))) el.hidden = !s.hasAccount;
    /* The form to set the wallet up by hand only when the automatic set-up
     * failed; while it runs, the status line says so. */
    $("zwsetup").hidden = s.hasAccount || autoFailed === null;
    $("zwhome").hidden = !s.hasAccount;
    renderInfo(s);
    say("zwserver", "");
    ($("zwserver") as HTMLInputElement).value = s.server;
    if (!s.hasAccount) {
      say("zwstatus", settingUp
        ? `Setting up the ${networkLabel(n).toLowerCase()} wallet of the board's account ${k.account}…`
        : autoFailed !== null
          ? `The automatic set-up failed: ${autoFailed}`
          : `No ${networkLabel(n).toLowerCase()} wallet on this device yet for the board's seed ZFP ${k.wallet.slice(0, 8)}…, account ${k.account}.`);
      return;
    }
    say("zwstatus", "");
    renderSync();
    try {
      const [b, a] = await Promise.all([call<Balances | null>("zw_balances"), call<Addresses>("zw_addresses")]);
      if (!sameKey(k, keyNow())) return;
      renderPools(b);
      addresses = a;
      recv.defaults = { unified: a.unified, transparent: a.transparent };
      const board = view.boardAccount;
      recv.board = board ? { ua: board.ua, tAddress: board.tAddress } : null;
      renderReceive();
      await loadHistory("zwrecent", 5);
      await loadContacts();
    } catch (e) {
      say("zwstatus", zErrorText(e));
    }
  }

  function renderInfo(s: Status): void {
    const kv = $("zwinfo");
    kv.replaceChildren();
    const row = (k: string, v: string) => {
      const dt = document.createElement("dt");
      dt.textContent = k;
      const dd = document.createElement("dd");
      dd.textContent = v;
      kv.append(dt, dd);
    };
    row("Network", networkLabel(net()));
    const k = keyNow();
    if (k) row("Seed fingerprint", k.wallet);
    row("Account", s.hasAccount ? `${s.accountIndex ?? "?"} (board ZFP ${s.zfp ?? "?"})` : "none");
    if (s.birthday !== null) row("Birthday", String(s.birthday));
    row("Data", s.dataDir);
  }

  /* ----------------------------------------------------- automatic set-up */

  /** Why the automatic set-up of the wallet on screen failed, if it did. */
  let autoFailed: string | null = null;
  let settingUp = false;

  /** Until no board request runs (the account read that announced this wallet). */
  async function boardFree(): Promise<void> {
    for (let i = 0; i < 100 && view.working; i++) await new Promise((r) => setTimeout(r, 100));
  }

  /**
   * The board's account was read: open its wallet, creating it when this
   * device has none (zcash-auto.ts), then sync. No clicks in the normal
   * path; a failure leaves the manual set-up on screen with the reason.
   */
  async function autoOpen(): Promise<void> {
    const a = view.boardAccount;
    const k = keyNow();
    if (!a || !k) return;
    await refresh();
    if (!sameKey(k, keyNow()) || !status) return;
    if (!status.hasAccount && autoFailed === null) {
      settingUp = true;
      say("zwstatus", "Setting up the wallet from the board…");
      try {
        await boardFree();
        const r = await view.busy("setting up the wallet", () => ensureWallet(a, {
          call: requireBoard(),
          invoke,
          say: (t) => { if (sameKey(k, keyNow())) say("zwstatus", t); },
          log: deps.log,
        }));
        if (r.birthdaySource === "tip") contacted();
      } catch (e) {
        autoFailed = zErrorText(e);
      } finally {
        settingUp = false;
      }
      if (!sameKey(k, keyNow())) return;
      await refresh();
    }
    if (status?.hasAccount && sameKey(k, keyNow())) {
      selectTab("ztab-wallet");
      scheduler.start();
    }
  }

  /* ------------------------------------------------------------- sync */

  let syncState: SyncState = { kind: "idle" };

  function renderSync(): void {
    const dot = $("zwsyncdot");
    const btn = $<HTMLButtonElement>("zwsync");
    const bar = $<HTMLProgressElement>("zwsyncbar");
    const st = syncState;
    dot.dataset["state"] = st.kind;
    btn.disabled = st.kind === "syncing";
    btn.textContent = st.kind === "error" ? "Retry" : "Sync now";
    bar.hidden = st.kind !== "syncing" || st.fraction === null;
    if (st.kind === "syncing" && st.fraction !== null) bar.value = st.fraction;
    if (st.kind === "syncing") say("zwsyncline", st.text);
    else if (st.kind === "error") say("zwsyncline", `Sync failed: ${st.text}`);
    else if (st.kind === "synced") {
      say("zwsyncline", st.height >= st.tip
        ? `Synced at block ${st.height} · ${st.at.toLocaleTimeString()}`
        : `Synced to block ${st.height} of ${st.tip} · ${st.at.toLocaleTimeString()}`);
    } else {
      say("zwsyncline", status?.fullyScanned == null ? "Not synced yet." : `Last synced to block ${status.fullyScanned} of ${status.chainTip ?? "?"}.`);
    }
  }

  /** One sync of the wallet on screen. The scheduler makes sure only one runs. */
  async function sync(): Promise<void> {
    const k = keyNow();
    if (!k || !status?.hasAccount) return;
    const n = k.network;
    syncState = { kind: "syncing", text: `Syncing: connecting to ${status.server}…`, fraction: null };
    renderSync();
    try {
      const done = await invoke<{ chainTip: number; fullyScanned: number; progress: number }>("zw_sync", { ...k });
      contacted();
      lastSync = { at: new Date(), text: `${n}: synced to block ${done.fullyScanned} of ${done.chainTip}` };
      syncState = { kind: "synced", height: done.fullyScanned, tip: done.chainTip, at: new Date() };
      deps.log(`zcash: ${n} wallet synced to ${done.fullyScanned}`);
    } catch (e) {
      const text = zErrorText(e);
      if (BUSY_RE.test(text)) {
        /* Another wallet command (a new address, a send being built) holds
         * the wallet for a moment: try again shortly, not an error. */
        syncState = { kind: "idle" };
        setTimeout(() => void scheduler.trigger(), 3000);
      } else {
        lastSync = { at: new Date(), text: `${n}: failed: ${text}` };
        syncState = { kind: "error", text };
      }
    }
    if (!sameKey(k, keyNow())) return;
    renderSync();
    await refresh();
    /* Receive was opened during the sync: hand out its new address now. */
    if (rotateAfterSync && paneOpen("zpane-receive")) {
      rotateAfterSync = false;
      await rotateReceive();
    }
  }

  const scheduler = new SyncScheduler(sync);

  void listener()?.<SyncEvent>("zw://sync", (e) => {
    const p = e.payload;
    if (p.network !== net() || syncState.kind !== "syncing") return;
    contacted();
    syncState = p.kind === "started"
      ? { kind: "syncing", text: `Syncing: blocks ${p.from} to ${p.tip}…`, fraction: 0 }
      : { kind: "syncing", text: `Syncing ${Math.floor(p.fraction * 100)}% · block ${p.end} of ${p.tip}`, fraction: p.fraction };
    renderSync();
  });
  void listener()?.<FinishEvent>("zw://finish", (e) => {
    if (e.payload.network === net()) say("zwsendstatus", e.payload.message);
  });

  /* In the background (Android, a minimised window) nothing syncs; back in
   * front it catches up if the last sync is stale. The camera is never left
   * on behind the user's back. */
  document.addEventListener("visibilitychange", () => {
    if (document.hidden) {
      scheduler.pause();
      stopScan();
    } else {
      scheduler.resume();
    }
  });

  /* ----------------------------------------------------------- import */

  async function importFromBoard(): Promise<void> {
    const n = net();
    await view.busy("importing the wallet", async () => {
      /* Read again even when an account is on screen: the board may have
       * taken a passphrase since, and the fingerprint it gives now is the
       * wallet this import goes into. */
      say("zwstatus", "Reading the account from the board…");
      const a = await view.readAccount();
      const typed = ($("zwbday") as HTMLInputElement).value.trim();
      const birthday = typed ? Number(typed) : a.birthday;
      if (birthday === null || !Number.isInteger(birthday) || birthday < 1) {
        throw new Error("The board has no birthday for this account. Enter the block height the seed was created at, or today's tip for a new seed.");
      }
      say("zwstatus", `Importing account ${a.account}, born at block ${birthday}…`);
      const r = await invoke<{ alreadyImported: boolean; birthday: number }>("zw_import", {
        network: n, ufvk: a.ufvk, seedFingerprint: a.seedFingerprint, account: a.account, ua: a.ua, birthday,
      });
      /* Recorded on the board for the next companion, as zecz does. */
      if (a.birthday !== r.birthday) {
        say("zwstatus", `Recording birthday ${r.birthday} on the board…`);
        await zSetBirthday(requireBoard(), a.account, n, r.birthday);
      }
      deps.log(`zcash: imported account ${a.account} into the ${n} wallet`);
    });
    if (n !== net()) return;
    autoFailed = null;
    await refresh();
    selectTab("ztab-wallet");
    scheduler.start();
  }

  /* ---------------------------------------------------------- receive */

  const recv = new ReceiveModel(() => call<NewAddress>("zw_next_address"));
  /** Receive opened while the wallet was busy syncing: rotate after it. */
  let rotateAfterSync = false;

  /** The QR code on screen: which button opened it, for which text. */
  let qrShown: { button: string; text: string } | null = null;

  function closeQr(): void {
    qrShown = null;
    $("zwqr").hidden = true;
    $("zwqr").replaceChildren();
    for (const id of ["zwuaqr", "zwtqr"]) {
      $(id).setAttribute("aria-expanded", "false");
      $(id).textContent = "Show QR code";
    }
  }

  function renderReceive(): void {
    const sv = recv.shielded();
    const tv = recv.transparent();
    const board = recv.mode === "board";
    $("zwrfresh").setAttribute("aria-pressed", String(!board));
    $("zwrboard").setAttribute("aria-pressed", String(board));
    say("zwuatitle", sv.title);
    say("zwua", sv.ua ?? "—");
    say("zwuanote", sv.note);
    $("zwuanew").hidden = board;
    say("zwttitle", tv.title);
    say("zwtaddr", tv.address ?? "(none)");
    say("zwtnote", tv.note);
    $("zwtnew").hidden = board;
    /* A QR code stays up across a background sync while its address is
     * still the one shown: a payer may be scanning it. */
    const now = qrShown?.button === "zwuaqr" ? sv.ua : qrShown?.button === "zwtqr" ? tv.address : null;
    if (!qrShown || now !== qrShown.text) closeQr();
  }

  /** A fresh shielded address: each time Receive opens, and on "New address". */
  async function rotateReceive(): Promise<void> {
    const k = keyNow();
    if (!k || !status?.hasAccount) return;
    const ok = await recv.rotate();
    if (!sameKey(k, keyNow())) return;
    if (ok) say("zwrecvstatus", "");
    else if (BUSY_RE.test(recv.lastError ?? "")) {
      rotateAfterSync = true;
      say("zwrecvstatus", "The wallet is syncing; a new address follows when it finishes. The one shown is this wallet's too.");
    } else {
      say("zwrecvstatus", zErrorText(new Error(recv.lastError ?? "")));
    }
    renderReceive();
  }

  function toggleQr(button: string, text: string | null | undefined): void {
    const qr = $("zwqr");
    const open = $(button).getAttribute("aria-expanded") === "true";
    closeQr();
    if (open || !text) return;
    qr.replaceChildren(deps.qr(text));
    qr.hidden = false;
    qrShown = { button, text };
    $(button).setAttribute("aria-expanded", "true");
    $(button).textContent = "Hide QR code";
  }

  /* ------------------------------------------------------------- send */

  /** Released when the proposal on screen is sent or dropped. */
  let releaseSync: (() => void) | null = null;

  function showProposal(p: Proposal): void {
    const kv = $("zwproposalkv");
    kv.replaceChildren();
    const row = (k: string, v: string) => {
      const dt = document.createElement("dt");
      dt.textContent = k;
      const dd = document.createElement("dd");
      dd.textContent = v;
      kv.append(dt, dd);
    };
    row("Network", networkLabel(net()));
    if (p.kind === "shield") row("What", "Shield every transparent coin of this account");
    for (const x of p.payments) {
      row("To", x.recipient);
      row("Amount", `${formatZec(x.amountZat)} ZEC (${x.pool}${x.hasMemo ? ", with memo" : ""})`);
    }
    for (const c of p.change) row("Change back to you", `${formatZec(c.valueZat)} ZEC (${c.pool})`);
    row("Fee", `${formatZec(p.feeZat)} ZEC`);
    row("Leaves the wallet", `${formatZec(p.totalZat)} ZEC`);
    $("zwmainconfirmrow").hidden = net() !== "main";
    ($("zwmainconfirm") as HTMLInputElement).checked = false;
    $("zwproposal").hidden = false;
    say("zwsendstatus", "Check this, then sign: the board shows the same recipients, amounts and fee, and its screen is the one to trust.");
  }

  function cancelProposal(): void {
    proposal = null;
    $("zwproposal").hidden = true;
    releaseSync?.();
    releaseSync = null;
  }

  async function propose(kind: "send" | "shield"): Promise<void> {
    cancelProposal();
    const k = keyNow();
    if (!k) { say("zwsendstatus", "Read the board's account first."); return; }
    const n = k.network;
    /* The wallet runs one command at a time: no sync may start between
     * this proposal and its finish, and a running one is waited out. */
    releaseSync = scheduler.hold();
    if (scheduler.running) say("zwsendstatus", "Waiting for the sync to finish…");
    await scheduler.whenIdle();
    say("zwsendstatus", kind === "send" ? "Building the transaction…" : "Building the shielding transaction…");
    try {
      const r = kind === "send"
        ? await invoke<{ proposal: Proposal; view: number[] }>("zw_propose_send", {
          ...k,
          to: resolveContact(($("zwto") as HTMLInputElement).value),
          amount: ($("zwamount") as HTMLInputElement).value,
          memo: ($("zwmemo") as HTMLTextAreaElement).value || null,
        })
        : await invoke<{ proposal: Proposal; view: number[] }>("zw_propose_shield", { ...k });
      if (!sameKey(k, keyNow())) { cancelProposal(); return; }
      proposal = { network: n, key: k, p: r.proposal, view: Uint8Array.from(r.view) };
      showProposal(r.proposal);
    } catch (e) {
      cancelProposal();
      say("zwsendstatus", zErrorText(e));
    }
  }

  async function signAndSend(): Promise<void> {
    const prop = proposal;
    const s = status;
    if (!prop || !s) return;
    if (!sameKey(prop.key, keyNow())) { cancelProposal(); return; }
    const mainOk = ($("zwmainconfirm") as HTMLInputElement).checked;
    if (prop.network === "main" && !mainOk) {
      say("zwsendstatus", "This is mainnet: tick the confirmation above first.");
      return;
    }
    const bar = $<HTMLProgressElement>("zwsendbar");
    $<HTMLButtonElement>("zwsignsend").disabled = true;
    try {
      const zsig = await view.busy("signing a send", async () => {
        say("zwsendstatus", "Checking that this board holds the wallet's seed…");
        const account = await boardMatches(s);
        bar.hidden = false;
        bar.value = 0;
        say("zwsendstatus", "Sending the transaction to the board…");
        const r = await zSign(requireBoard(), prop.view, account, prop.network, (p) => {
          if (p.phase === "upload") bar.value = p.sent / p.total;
          else {
            bar.hidden = true;
            say("zwsendstatus", "Review the transaction on the board. ACCEPT on its last page signs; anything else sends nothing.");
          }
        });
        if (r.entries.length === 0) throw new Error("the board returned no signatures");
        deps.log(`zcash: board approved sighash ${toHex(r.sighash)}`);
        return encodeZsig(r.sha256, r.entries);
      });
      say("zwsendstatus", "Signed on the board. Proving (the first time loads the proving key)…");
      const sent = await invoke<{ txid: string; broadcast: boolean }>("zw_finish", {
        ...prop.key, view: Array.from(prop.view), zsig: Array.from(zsig), mainnetConfirmed: mainOk,
      });
      cancelProposal();
      ($("zwamount") as HTMLInputElement).value = "";
      ($("zwmemo") as HTMLTextAreaElement).value = "";
      say("zwsendstatus", `Sent: ${sent.txid}. It shows as pending until it is mined.`);
      deps.log(`zcash: broadcast ${sent.txid} on ${prop.network}`);
      /* Right after a send or a shield: sync, so the balance and the
       * history show it. */
      void scheduler.trigger();
    } catch (e) {
      bar.hidden = true;
      say("zwsendstatus", zErrorText(e));
    } finally {
      $<HTMLButtonElement>("zwsignsend").disabled = false;
    }
  }

  /* ---------------------------------------------------- QR and requests */

  let scan: QrScan | null = null;

  function stopScan(): void {
    scan?.stop();
    scan = null;
    $("zwscanbox").hidden = true;
  }

  /**
   * Fill the form from a scanned or pasted address or ZIP 321 request.
   * Nothing is filled unless all of it fits: this network, one payment.
   */
  function fillFromRequest(text: string): boolean {
    let req;
    try {
      req = parsePaymentRequest(text);
    } catch (e) {
      say("zwscanstatus", `Not filled in: ${(e as Error).message}`);
      return false;
    }
    if (req.network !== net()) {
      say("zwscanstatus", `Not filled in: this is a ${networkLabel(req.network)} ${req.bare ? "address" : "payment request"}, and the wallet is on ${networkLabel(net())}. Switch the network first if that is what you meant.`);
      return false;
    }
    if (req.payments.length > 1) {
      say("zwscanstatus", `Not filled in: this request has ${req.payments.length} payments, and this wallet pays one recipient at a time.`);
      return false;
    }
    const p = req.payments[0]!;
    cancelProposal();
    ($("zwto") as HTMLInputElement).value = p.address;
    if (p.amount !== null) ($("zwamount") as HTMLInputElement).value = p.amount;
    const memo = $("zwmemo") as HTMLTextAreaElement;
    memo.value = p.memo ?? "";
    memo.dispatchEvent(new Event("input"));
    void describeTo();
    const from = [p.label, p.message].filter((x) => x).join(": ");
    say("zwscanstatus", `${req.bare ? "Address" : "Payment request"} filled in${from ? ` (${from})` : ""}. Check it, then Review; the board shows it again before signing.`);
    return true;
  }

  async function startScan(): Promise<void> {
    stopScan();
    if (!qrScanningAvailable()) {
      say("zwscanstatus", "This window cannot reach a camera. Paste the address or the zcash: link into To instead.");
      return;
    }
    $("zwscanbox").hidden = false;
    say("zwscanstatus", "Point the camera at a Zcash address or payment request QR code.");
    try {
      scan = await scanQr<string>(
        $<HTMLVideoElement>("zwscanvideo"),
        /* Only Zcash codes end the scan; another QR in shot is ignored. */
        (t) => (/^zcash:/i.test(t) || addressKind(t) !== null ? t : undefined),
        (t) => { stopScan(); fillFromRequest(t); },
        (m) => { stopScan(); say("zwscanstatus", m); },
      );
    } catch (e) {
      stopScan();
      const name = (e as { name?: string }).name;
      say("zwscanstatus", name === "NotAllowedError"
        ? "The camera was not allowed. Allow it for Zcash LeekWallet in the system settings, or paste the address."
        : name === "NotFoundError"
          ? "No camera found. Paste the address or the zcash: link into To instead."
          : `The camera did not start: ${(e as Error).message ?? String(e)}`);
    }
  }

  /* --------------------------------------------------------- contacts */

  let contacts: Contact[] = [];

  /** A contact's name typed into "To" means its address. */
  function resolveContact(to: string): string {
    const t = to.trim();
    return contacts.find((c) => c.name === t)?.address ?? t;
  }

  function payContact(c: Contact): void {
    cancelProposal();
    ($("zwto") as HTMLInputElement).value = c.address;
    say("zwscanstatus", "");
    void describeTo();
  }

  function renderContacts(): void {
    const list = $("zwcontacts");
    list.replaceChildren();
    const dl = $("zwcontactlist");
    dl.replaceChildren();
    const pick = $<HTMLSelectElement>("zwcontactpick");
    pick.replaceChildren();
    const first = document.createElement("option");
    first.value = "";
    first.textContent = contacts.length ? "Pay a contact…" : "No contacts yet";
    pick.append(first);
    pick.disabled = contacts.length === 0;
    contacts.forEach((c, i) => {
      const li = document.createElement("li");
      const name = document.createElement("strong");
      name.textContent = c.name;
      const addr = document.createElement("div");
      addr.className = "addr";
      addr.textContent = c.address;
      const pay = document.createElement("button");
      pay.type = "button";
      pay.className = "secondary";
      pay.textContent = "Pay";
      pay.addEventListener("click", () => {
        payContact(c);
        selectTab("ztab-send");
      });
      const del = document.createElement("button");
      del.type = "button";
      del.className = "ghost";
      del.textContent = "Remove";
      del.addEventListener("click", () => {
        void call<Contact[]>("zw_contact_remove", { name: c.name }).then((v) => { contacts = v; renderContacts(); },
          (e) => say("zwcstatus", zErrorText(e)));
      });
      li.append(name, addr, pay, del);
      list.append(li);
      const opt = document.createElement("option");
      opt.value = c.name;
      dl.append(opt);
      const o = document.createElement("option");
      o.value = String(i);
      o.textContent = c.name;
      pick.append(o);
    });
  }

  async function loadContacts(): Promise<void> {
    const k = keyNow();
    const v = await call<Contact[]>("zw_contacts");
    if (!sameKey(k, keyNow())) return;
    contacts = v;
    renderContacts();
  }

  /** What "To" holds, said under it; the memo box follows: no memo to a t-address. */
  async function describeTo(): Promise<void> {
    const to = resolveContact(($("zwto") as HTMLInputElement).value);
    const memo = $("zwmemo") as HTMLTextAreaElement;
    if (!to) { say("zwtokind", ""); memo.disabled = false; return; }
    /* The other network's address, said in words before the wallet's own
     * check (which refuses it too). */
    const local = addressKind(to);
    if (local && local.network !== net()) {
      say("zwtokind", `This is a ${networkLabel(local.network)} address; this wallet is on ${networkLabel(net())}.`);
      return;
    }
    try {
      const kind = await invoke<string>("zw_check_address", { network: net(), address: to });
      const named = contacts.find((c) => c.address === to)?.name;
      say("zwtokind", `${named ? `${named}: ` : ""}a ${kind} address on ${networkLabel(net())}.`);
      memo.disabled = kind === "transparent" || kind === "tex";
      if (memo.disabled) memo.value = "";
    } catch (e) {
      say("zwtokind", zErrorText(e));
    }
  }

  /* ----------------------------------------------------------- wiring */

  $("zwimport").addEventListener("click", () => void importFromBoard().catch((e) => say("zwstatus", zErrorText(e))));
  $("zwautoretry").addEventListener("click", () => { autoFailed = null; void autoOpen(); });
  $("zwtip").addEventListener("click", () => {
    void invoke<{ height: number }>("zw_tip", { network: net() }).then(
      (t) => { contacted(); ($("zwbday") as HTMLInputElement).value = String(t.height); },
      (e) => say("zwstatus", zErrorText(e)),
    );
  });
  $("zwsync").addEventListener("click", () => {
    if (scheduler.started) void scheduler.trigger();
    else scheduler.start();
  });
  $("zwhide").addEventListener("click", () => {
    hide = !hide;
    try { localStorage.setItem(HIDE_KEY, hide ? "1" : "0"); } catch { /* this run only */ }
    $("zwhide").setAttribute("aria-pressed", String(hide));
    $("zwhide").textContent = hide ? "Show balances" : "Hide balances";
    void refresh();
  });
  $("zwhide").setAttribute("aria-pressed", String(hide));
  $("zwhide").textContent = hide ? "Show balances" : "Hide balances";
  $("zwshieldgo").addEventListener("click", () => { selectTab("ztab-send"); void propose("shield"); });

  /* Receive: a fresh address each time the tab opens, as in Zodl. */
  $("ztab-receive").addEventListener("click", () => {
    if (recv.mode === "fresh") void rotateReceive();
  });
  $("zwrfresh").addEventListener("click", () => { recv.mode = "fresh"; renderReceive(); });
  $("zwrboard").addEventListener("click", () => {
    recv.mode = "board";
    renderReceive();
    say("zwrecvstatus", recv.board ? "" : "Read the board's account first (Board tab).");
  });
  $("zwuanew").addEventListener("click", () => void rotateReceive());
  $("zwtnew").addEventListener("click", () => {
    void recv.newTransparent().then((t) => {
      say("zwrecvstatus", t
        ? "A new transparent address. It is public on the chain; shield what it receives."
        : BUSY_RE.test(recv.lastError ?? "") ? "The wallet is syncing; try again when it finishes." : (recv.lastError ?? ""));
      renderReceive();
    });
  });

  $("zwuaverify").addEventListener("click", () => {
    const s = status;
    const ua = recv.shielded().verifyUa;
    if (!s || !ua) return;
    void view.busy("the address check", async () => {
      const account = await boardMatches(s);
      say("zwrecvstatus", "Compare the address on the board with the one here, then answer on the board.");
      await zShowAddress(requireBoard(), account, net(), ua);
      say("zwrecvstatus", "The board confirmed this address is its own, and you said it matches.");
    }).catch((e) => say("zwrecvstatus", zErrorText(e)));
  });
  $("zwtverify").addEventListener("click", () => {
    const s = status;
    const tv = recv.transparent();
    if (!s || !tv.verifyUa || !tv.address) return;
    void view.busy("the address check", async () => {
      const account = await boardMatches(s);
      say("zwrecvstatus", "The board shows the unified address that carries this transparent receiver: compare, then answer on the board.");
      const r = await zShowAddress(requireBoard(), account, net(), tv.verifyUa!);
      if (r.tAddress === null) throw new Error("The board confirmed the unified address but did not report its transparent receiver (older firmware).");
      if (r.tAddress !== tv.address) throw new Error(`The board's transparent receiver for that address is ${r.tAddress}, not ${tv.address}. Do not use it.`);
      say("zwrecvstatus", `The board confirmed ${tv.address} is its own transparent address.`);
    }).catch((e) => say("zwrecvstatus", zErrorText(e)));
  });
  $("zwuaqr").addEventListener("click", () => toggleQr("zwuaqr", recv.shielded().ua));
  $("zwtqr").addEventListener("click", () => toggleQr("zwtqr", recv.transparent().address));
  $("zwuacopy").addEventListener("click", () => void deps.copy(recv.shielded().ua ?? "").then(() => say("zwrecvstatus", "Copied the shielded address.")));
  $("zwtcopy").addEventListener("click", () => void deps.copy(recv.transparent().address ?? "").then(() => say("zwrecvstatus", "Copied the transparent address.")));

  $("zwscan").addEventListener("click", () => void startScan());
  $("zwscancancel").addEventListener("click", () => { stopScan(); say("zwscanstatus", "Scan cancelled."); });
  for (const t of Array.from(document.querySelectorAll<HTMLButtonElement>("#zcashpanel [role=tab]"))) {
    if (t.id !== "ztab-send") t.addEventListener("click", stopScan);
  }
  $<HTMLSelectElement>("zwcontactpick").addEventListener("change", (ev) => {
    const sel = ev.target as HTMLSelectElement;
    const c = contacts[Number(sel.value)];
    sel.value = "";
    if (sel.selectedIndex !== 0) sel.selectedIndex = 0;
    if (c) payContact(c);
  });
  $("zwto").addEventListener("change", () => {
    const to = ($("zwto") as HTMLInputElement).value.trim();
    /* A pasted zcash: link fills the whole form, as a scanned one does. */
    if (/^zcash:/i.test(to)) { fillFromRequest(to); return; }
    void describeTo();
  });
  $("zwmemo").addEventListener("input", () => {
    const n = memoBytes(($("zwmemo") as HTMLTextAreaElement).value);
    say("zwmemocount", `${n} / ${MEMO_BYTES} bytes${n > MEMO_BYTES ? " (too long)" : ""}`);
  });
  $("zwreview").addEventListener("click", () => void propose("send"));
  $("zwcancel").addEventListener("click", () => { cancelProposal(); say("zwsendstatus", "Cancelled; nothing was signed."); });
  $("zwsignsend").addEventListener("click", () => void signAndSend());

  $("zwhistrefresh").addEventListener("click", () => void loadHistory("zwhist").catch((e) => say("zwstatus", zErrorText(e))));
  $("ztab-activity").addEventListener("click", () => void loadHistory("zwhist").catch(() => undefined));

  $("zwcadd").addEventListener("click", () => {
    void call<Contact[]>("zw_contact_save", {
      name: ($("zwcname") as HTMLInputElement).value, address: ($("zwcaddr") as HTMLInputElement).value, replacing: null,
    }).then((v) => {
      contacts = v;
      renderContacts();
      ($("zwcname") as HTMLInputElement).value = "";
      ($("zwcaddr") as HTMLInputElement).value = "";
      say("zwcstatus", "Saved.");
    }, (e) => say("zwcstatus", zErrorText(e)));
  });

  const setServer = (server: string | null) => {
    say("zwsetstatus", server ? `Checking ${server}…` : "Back to the default server…");
    void invoke<{ server: string }>("zw_set_server", { network: net(), server }).then((r) => {
      if (server) contacted(); // a server it set was checked: it answered
      say("zwsetstatus", `Using ${r.server}.`);
      void refresh();
    }, (e) => say("zwsetstatus", zErrorText(e)));
  };
  $("zwserverset").addEventListener("click", () => setServer(($("zwserver") as HTMLInputElement).value.trim() || null));
  $("zwserverdefault").addEventListener("click", () => setServer(null));
  $("zwforget").addEventListener("click", () => {
    const btn = $("zwforget");
    if (btn.dataset["armed"] !== "1") {
      btn.dataset["armed"] = "1";
      btn.textContent = `Press again to forget the ${networkLabel(net()).toLowerCase()} wallet`;
      setTimeout(() => { btn.dataset["armed"] = ""; btn.textContent = "Forget this network's wallet"; }, 5000);
      return;
    }
    btn.dataset["armed"] = "";
    btn.textContent = "Forget this network's wallet";
    scheduler.stop();
    /* Forgotten on purpose: not created again behind the user's back. The
     * manual set-up (or "Try the automatic set-up again") brings it back. */
    autoFailed = "the wallet was forgotten in Settings.";
    void scheduler.whenIdle().then(() => call<void>("zw_forget")).then(() => { say("zwsetstatus", "Forgotten."); void refresh(); },
      (e) => say("zwsetstatus", zErrorText(e)));
  });

  /** Everything of the wallet that was on screen belongs to another one now. */
  function leaveWallet(): void {
    scheduler.stop();
    cancelProposal();
    stopScan();
    recv.reset();
    rotateAfterSync = false;
    autoFailed = null;
    syncState = { kind: "idle" };
    for (const id of ["zwsendstatus", "zwrecvstatus", "zwtokind", "zwscanstatus"]) say(id, "");
  }

  view.onChange(() => {
    leaveWallet();
    void refresh();
  });
  /* The board's account names the wallet: a new one (first read, another
   * seed after a passphrase, another account) is another wallet, opened (or
   * created) and synced by itself; none (lock, disconnect) is no wallet on
   * screen. */
  view.onAccount((a) => {
    leaveWallet();
    if (a) void autoOpen();
    else void refresh();
  });

  await refresh();
  selectTab("ztab-wallet");

  return {
    diagnostics: () => ({ status, lastContact, lastSync }),
  };
}
