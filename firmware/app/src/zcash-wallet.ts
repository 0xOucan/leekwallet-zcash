/**
 * The Zcash wallet screens (index.html #zpane-wallet and its siblings):
 * home with balance, pools, sync and the shield prompt; receive with
 * rotating shielded addresses; send with memo and review; history; the
 * address book; server and wallet settings.
 *
 * The screens follow Zodl's (the Zashi-derived Android wallet in android/,
 * MIT) as a feature reference; nothing of its code is used. The engine is
 * zecz (host/zecz-core) in src-tauri/src/zwallet.rs. The board is reached
 * the way the rest of the Companion reaches it, through the Client, so a
 * send is: propose there, sign here on the board, finish there.
 *
 * One wallet per network, seed fingerprint and account. The board's
 * account (zGetAccount, read when the home screen opens) names it: a BIP39
 * passphrase on the board is another seed, another fingerprint, and so
 * another wallet, kept apart on disk (src-tauri/src/zwallet.rs). Every
 * command names all three, and an answer for a wallet the user has since
 * left (another network, account or seed) is dropped rather than drawn.
 */

import {
  encodeZsig, explorerTx, formatZec, memoBytes, MEMO_BYTES, toHex, zErrorText, zGetAccount,
  zSetBirthday, zShowAddress, zSign, type ZNetwork,
} from "../packages/core/src/zcash.ts";
import type { Invoke } from "./tauri-transport.ts";
import { networkLabel, type ZcashDeps, type ZcashView } from "./zcash-view.ts";

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

const HIDE_KEY = "leek.zcash.hide";

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
    say("zwspendable", `${zec(b.spendable)} spendable now${b.synced ? "" : " (still syncing: more may appear)"}`);
    for (const [name, p] of [["Orchard", b.orchard], ["Ironwood", b.ironwood], ["Transparent", b.transparent]] as const) {
      if (p.total === 0 && p.pending === 0) continue;
      const dt = document.createElement("dt");
      dt.textContent = name;
      const dd = document.createElement("dd");
      dd.textContent = `${zec(p.total)}${p.pending ? ` (${zec(p.pending)} pending)` : ""}`;
      pools.append(dt, dd);
    }
    /* Zodl's prompt: transparent funds are public, and shielding is one
     * board review away. Only once they are spendable. */
    $("zwshield").hidden = b.transparent.spendable === 0;
    say("zwshieldamt", zec(b.transparent.spendable));
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
    addresses = null;
    for (const el of Array.from(document.querySelectorAll<HTMLElement>("#zcashpanel [data-account]"))) el.hidden = !s.hasAccount;
    $("zwsetup").hidden = s.hasAccount;
    $("zwhome").hidden = !s.hasAccount;
    renderInfo(s);
    say("zwserver", "");
    ($("zwserver") as HTMLInputElement).value = s.server;
    if (!s.hasAccount) {
      say("zwstatus", `No ${networkLabel(n).toLowerCase()} wallet on this device yet for the board's seed ZFP ${k.wallet.slice(0, 8)}…, account ${k.account}. A passphrase on the board is another seed: it gets its own wallet.`);
      return;
    }
    say("zwstatus", "");
    say("zwsyncline", s.fullyScanned === null ? "Never synced." : `Synced to block ${s.fullyScanned} of ${s.chainTip ?? "?"}.`);
    try {
      const [b, a] = await Promise.all([call<Balances | null>("zw_balances"), call<Addresses>("zw_addresses")]);
      if (!sameKey(k, keyNow())) return;
      renderPools(b);
      addresses = a;
      renderReceive(a.unified, a.transparent);
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

  /* ------------------------------------------------------------- sync */

  let syncing = false;
  async function sync(): Promise<void> {
    if (syncing) return;
    const k = keyNow();
    if (!k) return;
    syncing = true;
    const n = k.network;
    const bar = $<HTMLProgressElement>("zwsyncbar");
    bar.hidden = false;
    bar.value = 0;
    $<HTMLButtonElement>("zwsync").disabled = true;
    say("zwsyncline", `Connecting to ${status?.server ?? "the server"}…`);
    try {
      const done = await invoke<{ chainTip: number; fullyScanned: number; progress: number }>("zw_sync", { ...k });
      contacted();
      lastSync = { at: new Date(), text: `${n}: synced to block ${done.fullyScanned} of ${done.chainTip}` };
      if (sameKey(k, keyNow())) say("zwsyncline", `Synced to block ${done.fullyScanned} of ${done.chainTip}.`);
      deps.log(`zcash: ${n} wallet synced to ${done.fullyScanned}`);
    } catch (e) {
      lastSync = { at: new Date(), text: `${n}: failed: ${zErrorText(e)}` };
      if (sameKey(k, keyNow())) say("zwsyncline", `Sync failed: ${zErrorText(e)}`);
    } finally {
      syncing = false;
      bar.hidden = true;
      $<HTMLButtonElement>("zwsync").disabled = false;
      if (sameKey(k, keyNow())) await refresh();
    }
  }

  void listener()?.<SyncEvent>("zw://sync", (e) => {
    const p = e.payload;
    if (p.network !== net()) return;
    contacted();
    if (p.kind === "started") {
      say("zwsyncline", `Scanning from block ${p.from} to ${p.tip}…`);
    } else {
      $<HTMLProgressElement>("zwsyncbar").value = p.fraction;
      say("zwsyncline", `Scanned to block ${p.end} of ${p.tip} (${Math.floor(p.fraction * 100)}%)`);
    }
  });
  void listener()?.<FinishEvent>("zw://finish", (e) => {
    if (e.payload.network === net()) say("zwsendstatus", e.payload.message);
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
    await refresh();
    selectTab("ztab-wallet");
    void sync();
  }

  /* ---------------------------------------------------------- receive */

  function renderReceive(ua: string, t: string | null): void {
    say("zwua", ua);
    say("zwtaddr", t ?? "(none)");
    $("zwqr").hidden = true;
    $("zwqr").replaceChildren();
  }

  function toggleQr(button: string, text: string | null | undefined): void {
    const qr = $("zwqr");
    const open = $(button).getAttribute("aria-expanded") === "true";
    for (const id of ["zwuaqr", "zwtqr"]) {
      $(id).setAttribute("aria-expanded", "false");
      $(id).textContent = "Show QR code";
    }
    if (open || !text) {
      qr.hidden = true;
      qr.replaceChildren();
      return;
    }
    qr.replaceChildren(deps.qr(text));
    qr.hidden = false;
    $(button).setAttribute("aria-expanded", "true");
    $(button).textContent = "Hide QR code";
  }

  /* ------------------------------------------------------------- send */

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
  }

  async function propose(kind: "send" | "shield"): Promise<void> {
    cancelProposal();
    const k = keyNow();
    if (!k) { say("zwsendstatus", "Read the board's account first."); return; }
    const n = k.network;
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
      if (!sameKey(k, keyNow())) return;
      proposal = { network: n, key: k, p: r.proposal, view: Uint8Array.from(r.view) };
      showProposal(r.proposal);
    } catch (e) {
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
      void refresh();
    } catch (e) {
      bar.hidden = true;
      say("zwsendstatus", zErrorText(e));
    } finally {
      $<HTMLButtonElement>("zwsignsend").disabled = false;
    }
  }

  /* --------------------------------------------------------- contacts */

  let contacts: Contact[] = [];

  /** A contact's name typed into "To" means its address. */
  function resolveContact(to: string): string {
    const t = to.trim();
    return contacts.find((c) => c.name === t)?.address ?? t;
  }

  function renderContacts(): void {
    const list = $("zwcontacts");
    list.replaceChildren();
    const dl = $("zwcontactlist");
    dl.replaceChildren();
    for (const c of contacts) {
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
        ($("zwto") as HTMLInputElement).value = c.address;
        void describeTo();
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
    }
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
    try {
      const kind = await invoke<string>("zw_check_address", { network: net(), address: to });
      const named = contacts.find((c) => c.address === to)?.name;
      say("zwtokind", `${named ? `${named}: ` : ""}a ${kind} address on ${networkLabel(net())}.`);
      memo.disabled = kind === "transparent";
      if (memo.disabled) memo.value = "";
    } catch (e) {
      say("zwtokind", zErrorText(e));
    }
  }

  /* ----------------------------------------------------------- wiring */

  $("zwimport").addEventListener("click", () => void importFromBoard().catch((e) => say("zwstatus", zErrorText(e))));
  $("zwtip").addEventListener("click", () => {
    void invoke<{ height: number }>("zw_tip", { network: net() }).then(
      (t) => { contacted(); ($("zwbday") as HTMLInputElement).value = String(t.height); },
      (e) => say("zwstatus", zErrorText(e)),
    );
  });
  $("zwsync").addEventListener("click", () => void sync());
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

  $("zwuaverify").addEventListener("click", () => {
    const s = status;
    const ua = addresses?.unified;
    if (!s || !ua) return;
    void view.busy("the address check", async () => {
      const account = await boardMatches(s);
      say("zwrecvstatus", "Compare the address on the board with the one here, then answer on the board.");
      await zShowAddress(requireBoard(), account, net(), ua);
      say("zwrecvstatus", "The board confirmed this address is its own, and you said it matches.");
    }).catch((e) => say("zwrecvstatus", zErrorText(e)));
  });
  $("zwuanew").addEventListener("click", () => {
    void call<{ unified: string; transparent: string | null }>("zw_next_address").then((a) => {
      if (addresses) addresses = { ...addresses, unified: a.unified };
      renderReceive(a.unified, addresses?.transparent ?? a.transparent);
      say("zwrecvstatus", "A new shielded address. Payments to any earlier one still arrive.");
    }, (e) => say("zwrecvstatus", zErrorText(e)));
  });
  $("zwuaqr").addEventListener("click", () => toggleQr("zwuaqr", addresses?.unified));
  $("zwtqr").addEventListener("click", () => toggleQr("zwtqr", addresses?.transparent));
  $("zwuacopy").addEventListener("click", () => void deps.copy(addresses?.unified ?? "").then(() => say("zwrecvstatus", "Copied the shielded address.")));
  $("zwtcopy").addEventListener("click", () => void deps.copy(addresses?.transparent ?? "").then(() => say("zwrecvstatus", "Copied the transparent address.")));

  $("zwto").addEventListener("change", () => void describeTo());
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
    void call<void>("zw_forget").then(() => { say("zwsetstatus", "Forgotten."); void refresh(); },
      (e) => say("zwsetstatus", zErrorText(e)));
  });

  view.onChange(() => {
    cancelProposal();
    say("zwsendstatus", "");
    say("zwrecvstatus", "");
    say("zwtokind", "");
    void refresh();
  });
  /* The board's account names the wallet: a new one (first read, another
   * seed after a passphrase, another account) is another wallet, and none
   * (lock, disconnect) is no wallet on screen. */
  view.onAccount(() => {
    cancelProposal();
    say("zwsendstatus", "");
    say("zwrecvstatus", "");
    void refresh();
  });

  await refresh();
  selectTab("ztab-wallet");

  return {
    diagnostics: () => ({ status, lastContact, lastSync }),
  };
}
