// LeekWallet Zcash desktop: one screen over the Rust backend.
//
// The webview holds no wallet logic. It calls the commands in
// src-tauri/src/lib.rs and draws what they return; every amount arrives in
// zatoshis and is formatted here only for display.

// ------------------------------------------------------------- tauri glue

type Invoke = <T>(cmd: string, args?: Record<string, unknown>) => Promise<T>;
type Listen = <T>(event: string, cb: (e: { payload: T }) => void) => Promise<() => void>;

interface TauriGlobal {
  core?: { invoke?: Invoke };
  event?: { listen?: Listen };
}

// withGlobalTauri is on in tauri.conf.json, as in firmware/app, so there
// is no @tauri-apps/api bundle to ship.
const tauri = (window as unknown as { __TAURI__?: TauriGlobal }).__TAURI__;

function invoke<T>(cmd: string, args?: Record<string, unknown>): Promise<T> {
  const f = tauri?.core?.invoke;
  if (!f) return Promise.reject(new Error("not running inside the LeekWallet Zcash app"));
  return f<T>(cmd, args);
}

// ------------------------------------------------------------ backend types

interface ProfileStatus {
  network: "test" | "main" | string;
  server: string;
  dataDir: string;
  hasAccount: boolean;
  birthday: number | null;
  fullyScanned: number | null;
  chainTip: number | null;
  /** Imported from a LeekWallet: the device signs for it. */
  hardware: boolean;
  zfp: string | null;
}

interface PortInfo {
  name: string;
  description: string;
  likelyDevice: boolean;
}

interface BleDevice {
  id: string;
  name: string | null;
}

interface Paired {
  transport: "usb" | "ble" | string;
  port: string;
  account: number;
  zfp: string;
  ua: string;
  tAddress: string;
  birthday: number | null;
  matchesWallet: boolean | null;
}

interface DeviceStatus {
  step: string;
  message: string;
  done: number | null;
  total: number | null;
}

interface DeviceImported {
  alreadyImported: boolean;
  birthday: number;
  birthdayFrom: "user" | "device" | "wallet";
  birthdayStored: boolean | null;
  zfp: string;
  addresses: Addresses;
}

interface Proposal {
  id: string;
  /** "shield": no payments; the change is what gets shielded. */
  kind: "send" | "shield";
  targetHeight: number;
  payments: { recipient: string; amountZat: number; pool: string; hasMemo: boolean }[];
  change: { valueZat: number; pool: string }[];
  feeZat: number;
  totalZat: number;
  signerViewBytes: number;
  compact: boolean;
}

interface Sent {
  txid: string;
  signatures: number;
  size: number;
  broadcast: boolean;
}

interface Addresses {
  unified: string;
  transparent: string | null;
  orchardOnly: string;
}

interface Tx {
  txid: string;
  minedHeight: number | null;
  /** Unix seconds. */
  blockTime: number | null;
  expiryHeight: number | null;
  expired: boolean;
  /** "self": sent to the wallet's own address; amountZat is the fee. */
  kind: "received" | "sent" | "self" | "shielded" | "internal";
  amountZat: number;
  deltaZat: number;
  /** Only when this wallet paid it. */
  feeZat: number | null;
  memos: string[];
  recipients: { address: string; valueZat: number }[];
}

interface Contact {
  name: string;
  address: string;
}

interface NewAddress {
  unified: string;
  /** Decimal string: it can exceed 2^53. */
  diversifierIndex: string;
  transparent: string | null;
}

interface Imported {
  alreadyImported: boolean;
  birthday: number;
  addresses: Addresses;
}

type SyncProgress =
  | { kind: "started"; server: string; tip: number; from: number }
  | { kind: "scanned"; start: number; end: number; tip: number; fraction: number };

interface SyncDone {
  chainTip: number;
  fullyScanned: number;
  progress: number;
}

interface Pool {
  total: number;
  spendable: number;
  pending: number;
}

interface Balances {
  chainTip: number;
  fullyScanned: number;
  synced: boolean;
  orchard: Pool;
  ironwood: Pool;
  transparent: Pool;
  total: number;
  spendable: number;
}

interface Tip {
  server: string;
  chainName: string;
  height: number;
  vendor: string;
  version: string;
}

interface NetworkSettings {
  network: "test" | "main" | string;
  server: string;
  defaultServer: string;
  customServer: boolean;
  /** LEEKZ_NETWORK / LEEKZ_SERVER chose this run's network or server. */
  fromEnvironment: boolean;
}

// ------------------------------------------------------------------ helpers

function $<T extends HTMLElement = HTMLElement>(id: string): T {
  const el = document.getElementById(id);
  if (!el) throw new Error(`#${id} missing`);
  return el as T;
}

function errText(e: unknown): string {
  return e instanceof Error ? e.message : String(e);
}

/** Zatoshis as ZEC with all eight decimals, so nothing is rounded away. */
export function formatZec(zat: number, ticker: string): string {
  const whole = Math.floor(zat / 1e8);
  const frac = String(zat % 1e8).padStart(8, "0");
  return `${whole.toLocaleString("en-US")}.${frac} ${ticker}`;
}

/** Groups of four, so the address wraps between groups and is easy to compare. */
export function chunk(s: string): string {
  return s.replace(/(.{4})/g, "$1 ").trim();
}

function setConn(state: "connected" | "connecting" | "error" | "disconnected", text: string): void {
  $("dot").dataset["state"] = state;
  $("conn").textContent = text;
}

let ticker = "TAZ";
let profile: ProfileStatus | null = null;
let paired: Paired | null = null;

// ------------------------------------------------------------------- render

async function refresh(): Promise<void> {
  const s = await invoke<ProfileStatus>("profile_status");
  profile = s;
  ticker = s.network === "main" ? "ZEC" : "TAZ";
  $("send-amount-label").textContent = `Amount (${ticker})`;
  const net = $("network");
  net.textContent = s.network === "main" ? "mainnet" : "testnet";
  net.dataset["net"] = s.network;
  $("server").textContent = s.server;
  $("server").title = `lightwalletd server ${s.server}; wallet data in ${s.dataDir}`;
  const main = s.network === "main";
  $("netnote").hidden = main;
  $("mainnote").hidden = !main;
  $("ufvk-hrp").textContent = main ? "uview1…" : "uviewtest1…";
  document.querySelectorAll<HTMLElement>(".confirm-main").forEach((el) => (el.hidden = !main));

  // A watch-only account has no device; an empty wallet offers both ways in.
  $("device-panel").hidden = s.hasAccount && !s.hardware;
  $("import-panel").hidden = s.hasAccount;
  $("sync-panel").hidden = !s.hasAccount;
  $("balance-panel").hidden = !s.hasAccount;
  $("receive-panel").hidden = !s.hasAccount;
  $("send-panel").hidden = !(s.hasAccount && s.hardware);
  $("history-panel").hidden = !s.hasAccount;
  $("contacts-panel").hidden = !s.hasAccount;
  $("swap-panel").hidden = !s.hasAccount;
  $("verify-btn").hidden = !s.hardware;
  renderDevice();
  if (!s.hasAccount) {
    setConn("disconnected", "No account yet");
    return;
  }

  if (s.fullyScanned !== null && s.chainTip !== null) {
    showScanned(s.fullyScanned, s.chainTip, s.birthday ?? s.fullyScanned);
  } else {
    $("sync-status").textContent = `Not synced yet. Scanning starts at the birthday, block ${s.birthday ?? "?"}.`;
  }
  setConn("connected", s.hardware ? `LeekWallet account · ZFP ${s.zfp ?? "?"}` : "Watch-only account");
  await Promise.all([renderAddresses(), renderBalances(), loadContacts(), renderHistory(), loadSwapScreen()]);
}

/** The device panel: the link chooser, or the connected device. */
function renderDevice(): void {
  const s = profile;
  const p = paired;
  $("device-pick").hidden = p !== null;
  $("device-connected").hidden = p === null;
  $("device-hint").hidden = p !== null;
  if (p) {
    const via = p.transport === "ble" ? "Bluetooth" : `USB ${p.port}`;
    let line = `Connected over ${via} · ZFP ${p.zfp}`;
    if (p.matchesWallet === false) line += " · not this wallet's seed";
    $("device-line").textContent = line;
  }
  // Import once paired and the wallet is still empty.
  const importing = p !== null && s !== null && !s.hasAccount;
  $("device-import-form").hidden = !importing;
  if (importing && p) {
    $("dev-zfp").textContent = p.zfp;
    $("dev-ua").textContent = chunk(p.ua);
    const bday = $<HTMLInputElement>("dev-birthday");
    if (p.birthday !== null && bday.value === "") bday.value = String(p.birthday);
    $("dev-bday-label").textContent =
      p.birthday !== null
        ? `Birthday height (from the device: ${p.birthday})`
        : "Birthday height (the first block that can hold its funds; the device does not know it)";
  }
  if (p?.matchesWallet === false) {
    $("device-err").textContent = `This LeekWallet holds another seed (ZFP ${p.zfp}) than this wallet's account (ZFP ${s?.zfp ?? "?"}).`;
  } else if (p) {
    $("device-err").textContent = "";
  }
}

function showScanned(scanned: number, tip: number, birthday: number): void {
  const span = Math.max(1, tip - birthday);
  const bar = $<HTMLProgressElement>("sync-bar");
  bar.value = Math.min(1, Math.max(0, (scanned - birthday) / span));
  $("sync-status").textContent =
    scanned >= tip ? `Up to date at block ${tip}.` : `Scanned to block ${scanned} of ${tip}.`;
}

/**
 * The address the receive card shows: the default one at start, or the one
 * "New address" rotated to, for the rest of the session.
 */
let rotated: NewAddress | null = null;

async function renderAddresses(): Promise<void> {
  const a = await invoke<Addresses>("addresses");
  showUa(rotated ? rotated.unified : a.unified, rotated);
  $("taddr-box").hidden = a.transparent === null;
  if (a.transparent !== null) {
    $("taddr").textContent = chunk(a.transparent);
    $("taddr").dataset["raw"] = a.transparent;
  }
}

function showUa(unified: string, fresh: NewAddress | null): void {
  const ua = $("ua");
  ua.textContent = chunk(unified);
  ua.dataset["raw"] = unified;
  $("ua-label").textContent =
    fresh === null
      ? "Unified address (Orchard and transparent receivers)."
      : fresh.transparent === null
        ? "A new unified address (Orchard only: the transparent receivers in reserve are all handed out until one is paid)."
        : "A new unified address (Orchard and transparent receivers). Earlier addresses keep working.";
  $("verify-status").textContent = "";
  $("verify-err").textContent = "";
  void drawQr(unified);
}

async function drawQr(text: string): Promise<void> {
  const img = $<HTMLImageElement>("ua-qr");
  try {
    const svg = await invoke<string>("qr_svg", { text });
    // The address on screen may have changed while the QR was drawn.
    if (($("ua").dataset["raw"] ?? "") !== text) return;
    img.src = `data:image/svg+xml;charset=utf-8,${encodeURIComponent(svg)}`;
    img.hidden = false;
  } catch {
    img.hidden = true;
  }
}

async function renderBalances(): Promise<void> {
  const b = await invoke<Balances | null>("balances");
  const set = (id: string, zat: number | null) => {
    $(id).textContent = zat === null ? "—" : formatZec(zat, ticker);
  };
  set("bal-orchard", b ? b.orchard.total : null);
  set("bal-ironwood", b ? b.ironwood.total : null);
  set("bal-transparent", b ? b.transparent.total : null);
  set("bal-total", b ? b.total : null);
  set("bal-spendable", b ? b.spendable : null);
  const note = $("balance-note");
  if (!b) note.textContent = "Sync to see the balance.";
  else if (!b.synced) note.textContent = "Not fully synced; the balance may be incomplete.";
  else {
    const pending = b.orchard.pending + b.ironwood.pending + b.transparent.pending;
    note.textContent = pending > 0 ? `${formatZec(pending, ticker)} waiting for confirmations.` : "";
  }
  // Shielding is signed on the device: a LeekWallet account only. The
  // backend decides whether there is enough to shield, and says why not.
  const canShield = b !== null && b.transparent.total > 0 && profile?.hardware === true;
  $("shield-row").hidden = !canShield || !$("shield-review").hidden || !$("shielded").hidden;
  if (!canShield) $("shield-err").textContent = "";
}

// ------------------------------------------------------------------ actions

$<HTMLFormElement>("import-form").addEventListener("submit", async (ev) => {
  ev.preventDefault();
  const ufvkEl = $<HTMLTextAreaElement>("ufvk");
  const bdayEl = $<HTMLInputElement>("birthday");
  const err = $("import-err");
  err.textContent = "";
  const ufvk = ufvkEl.value.trim();
  const birthday = Number(bdayEl.value);
  ufvkEl.setAttribute("aria-invalid", String(ufvk === ""));
  const badBday = !Number.isInteger(birthday) || birthday <= 0;
  bdayEl.setAttribute("aria-invalid", String(badBday));
  if (ufvk === "" || badBday) {
    err.textContent = "Enter a viewing key and a birthday height.";
    return;
  }
  const btn = $<HTMLButtonElement>("import-btn");
  btn.disabled = true;
  setConn("connecting", "Importing…");
  try {
    await invoke<Imported>("init_watch_only", { ufvk, birthday });
    ufvkEl.value = "";
    await refresh();
  } catch (e) {
    err.textContent = errText(e);
    setConn("error", "Import failed");
  } finally {
    btn.disabled = false;
  }
});

$("tip-btn").addEventListener("click", async () => {
  const btn = $<HTMLButtonElement>("tip-btn");
  btn.disabled = true;
  $("import-err").textContent = "";
  try {
    const t = await invoke<Tip>("tip");
    $<HTMLInputElement>("birthday").value = String(t.height);
  } catch (e) {
    $("import-err").textContent = errText(e);
  } finally {
    btn.disabled = false;
  }
});

$("sync-btn").addEventListener("click", async () => {
  const btn = $<HTMLButtonElement>("sync-btn");
  const err = $("sync-err");
  const bar = $<HTMLProgressElement>("sync-bar");
  btn.disabled = true;
  err.textContent = "";
  setConn("connecting", "Syncing…");
  $("sync-status").textContent = "Connecting to lightwalletd…";
  try {
    const done = await invoke<SyncDone>("sync");
    bar.value = done.progress;
    $("sync-status").textContent = `Up to date at block ${done.chainTip}.`;
    await Promise.all([renderBalances(), renderHistory()]);
    setConn("connected", "Synced");
  } catch (e) {
    err.textContent = errText(e);
    setConn("error", "Sync failed");
  } finally {
    btn.disabled = false;
  }
});

void tauri?.event?.listen?.<SyncProgress>("sync://progress", ({ payload: p }) => {
  if (p.kind === "started") {
    $("sync-status").textContent = `Scanning from block ${p.from} to the tip at ${p.tip}…`;
  } else {
    $<HTMLProgressElement>("sync-bar").value = p.fraction;
    $("sync-status").textContent = `Scanned ${p.start}–${p.end - 1} (${Math.floor(p.fraction * 100)}%), tip ${p.tip}`;
  }
});

/** Copies the raw text of `source` (its data-raw) and says so on `btn`. */
async function copyFrom(btn: HTMLElement, source: HTMLElement): Promise<void> {
  const raw = source.dataset["raw"] ?? source.textContent ?? "";
  try {
    await navigator.clipboard.writeText(raw);
    btn.textContent = "Copied";
    setTimeout(() => (btn.textContent = "Copy"), 1500);
  } catch {
    // Clipboard denied: select the text so it can be copied by hand.
    const r = document.createRange();
    r.selectNodeContents(source);
    getSelection()?.removeAllRanges();
    getSelection()?.addRange(r);
  }
}

$("copy-ua").addEventListener("click", () => void copyFrom($("copy-ua"), $("ua")));
$("copy-taddr").addEventListener("click", () => void copyFrom($("copy-taddr"), $("taddr")));

$("new-addr-btn").addEventListener("click", async () => {
  const btn = $<HTMLButtonElement>("new-addr-btn");
  btn.disabled = true;
  $("verify-err").textContent = "";
  try {
    rotated = await invoke<NewAddress>("next_address");
    showUa(rotated.unified, rotated);
  } catch (e) {
    $("verify-err").textContent = errText(e);
  } finally {
    btn.disabled = false;
  }
});

// ------------------------------------------------------------------- device

// Device calls wait on a person: the backend runs one at a time and reports
// each step on `device://status`. Whichever flow is running owns the status
// line those steps are written to.
let statusLine = "device-status";
let uploadBar: HTMLProgressElement | null = null;

void tauri?.event?.listen?.<DeviceStatus>("device://status", ({ payload: st }) => {
  $(statusLine).textContent = st.message;
  if (uploadBar && st.step === "upload" && st.total) {
    uploadBar.hidden = false;
    uploadBar.value = (st.done ?? 0) / st.total;
  }
});

void tauri?.event?.listen?.<string>("device://passkey", ({ payload: passkey }) => {
  $("passkey").textContent = `${passkey.slice(0, 3)} ${passkey.slice(3)}`;
  $("pairing").hidden = false;
});

/** Runs one device flow with its status line, its button disabled. */
async function deviceFlow<T>(line: string, btn: HTMLButtonElement, f: () => Promise<T>): Promise<T> {
  statusLine = line;
  btn.disabled = true;
  $(line).textContent = "";
  try {
    return await f();
  } finally {
    btn.disabled = false;
  }
}

/** A device call failed: if the backend closed the session, show the chooser again. */
async function afterDeviceError(): Promise<void> {
  try {
    paired = await invoke<Paired | null>("device_session");
  } catch {
    /* busy: leave the panel as it is */
  }
  renderDevice();
}

/** The link chooser: serial ports (the board's first), then Bluetooth. */
let bleFound: BleDevice[] = [];

async function loadLinks(): Promise<void> {
  const sel = $<HTMLSelectElement>("link");
  const keep = sel.value;
  let ports: PortInfo[] = [];
  try {
    ports = await invoke<PortInfo[]>("list_ports");
  } catch (e) {
    $("device-err").textContent = errText(e);
  }
  sel.replaceChildren();
  const add = (value: string, label: string) => {
    const o = document.createElement("option");
    o.value = value;
    o.textContent = label;
    sel.append(o);
  };
  for (const p of ports) {
    add(`usb:${p.name}`, `USB · ${p.name} · ${p.description}${p.likelyDevice ? " (LeekWallet?)" : ""}`);
  }
  if (ports.length === 0) add("usb:", "USB · no serial port found (plug in the board, then Refresh)");
  for (const d of bleFound) add(`ble:${d.id}`, `Bluetooth · ${d.name ?? "LeekWallet"} · ${d.id}`);
  add("ble:", "Bluetooth · first LeekWallet found");
  if (Array.from(sel.options).some((o) => o.value === keep)) sel.value = keep;
}

$("link-refresh").addEventListener("click", () => void loadLinks());

$("ble-scan").addEventListener("click", async () => {
  const btn = $<HTMLButtonElement>("ble-scan");
  $("device-err").textContent = "";
  try {
    await deviceFlow("device-status", btn, async () => {
      $("device-status").textContent = "Scanning for LeekWallets over Bluetooth…";
      bleFound = await invoke<BleDevice[]>("ble_scan");
      $("device-status").textContent =
        bleFound.length === 0
          ? "No LeekWallet is advertising. Select Bluetooth as the link on the device, then scan again."
          : `Found ${bleFound.length} over Bluetooth.`;
    });
    await loadLinks();
    if (bleFound[0]) $<HTMLSelectElement>("link").value = `ble:${bleFound[0].id}`;
  } catch (e) {
    $("device-status").textContent = "";
    $("device-err").textContent = errText(e);
  }
});

$("pair-btn").addEventListener("click", async () => {
  const choice = $<HTMLSelectElement>("link").value;
  const sep = choice.indexOf(":");
  const transport = choice.slice(0, sep);
  const port = choice.slice(sep + 1);
  if (transport === "usb" && port === "") {
    $("device-err").textContent = "No serial port to connect to. Plug in the board and press Refresh, or choose Bluetooth.";
    return;
  }
  $("device-err").textContent = "";
  $("passkey").textContent = "";
  setConn("connecting", "Connecting to LeekWallet…");
  try {
    paired = await deviceFlow("device-status", $<HTMLButtonElement>("pair-btn"), () =>
      invoke<Paired>("device_pair", { transport, port: port === "" ? null : port, account: 0 }),
    );
    $("device-status").textContent = "";
    $("pairing").hidden = true;
    await refresh();
  } catch (e) {
    $("pairing").hidden = true;
    $("device-status").textContent = "";
    $("device-err").textContent = errText(e);
    setConn("error", "Not connected");
    await afterDeviceError();
  }
});

$("unpair-btn").addEventListener("click", async () => {
  try {
    await invoke("device_disconnect");
    paired = null;
    $("device-status").textContent = "";
    renderDevice();
  } catch (e) {
    $("device-err").textContent = errText(e);
  }
});

$<HTMLFormElement>("device-import-form").addEventListener("submit", async (ev) => {
  ev.preventDefault();
  const bdayEl = $<HTMLInputElement>("dev-birthday");
  const err = $("dev-import-err");
  err.textContent = "";
  const raw = bdayEl.value.trim();
  const birthday = raw === "" ? null : Number(raw);
  const bad = birthday !== null && (!Number.isInteger(birthday) || birthday <= 0);
  bdayEl.setAttribute("aria-invalid", String(bad));
  if (bad || (birthday === null && paired?.birthday == null)) {
    bdayEl.setAttribute("aria-invalid", "true");
    err.textContent = "Enter a birthday height: the first block that can hold this account's funds.";
    return;
  }
  setConn("connecting", "Importing…");
  try {
    const r = await deviceFlow("device-status", $<HTMLButtonElement>("dev-import-btn"), () =>
      invoke<DeviceImported>("device_import", { birthday }),
    );
    const where =
      r.birthdayStored === true
        ? " It is now recorded on the device too."
        : r.birthdayStored === false
          ? " The device keeps it until it locks (passphrase or temporary seed)."
          : "";
    $("device-status").textContent = `Imported ZFP ${r.zfp}, born at block ${r.birthday}.${where} Sync to see the balance.`;
    bdayEl.value = "";
    await refresh();
  } catch (e) {
    err.textContent = errText(e);
    setConn("error", "Import failed");
    await afterDeviceError();
  }
});

$("dev-tip-btn").addEventListener("click", async () => {
  const btn = $<HTMLButtonElement>("dev-tip-btn");
  btn.disabled = true;
  try {
    const t = await invoke<Tip>("tip");
    $<HTMLInputElement>("dev-birthday").value = String(t.height);
  } catch (e) {
    $("dev-import-err").textContent = errText(e);
  } finally {
    btn.disabled = false;
  }
});

$("verify-btn").addEventListener("click", async () => {
  $("verify-err").textContent = "";
  if (!paired) {
    $("verify-err").textContent = "Connect the LeekWallet first (above).";
    return;
  }
  const ua = $("ua").dataset["raw"] ?? "";
  try {
    await deviceFlow("verify-status", $<HTMLButtonElement>("verify-btn"), () => invoke("verify_address", { ua }));
    $("verify-status").textContent = "Confirmed on the device: this address is your LeekWallet's.";
  } catch (e) {
    $("verify-status").textContent = "";
    $("verify-err").textContent = errText(e);
    await afterDeviceError();
  }
});

// --------------------------------------------------------------------- send

let proposal: Proposal | null = null;

function showSendStage(stage: "form" | "review" | "sent"): void {
  $("send-form").hidden = stage !== "form";
  $("review").hidden = stage !== "review";
  $("sent").hidden = stage !== "sent";
}

function renderReview(p: Proposal): void {
  const kv = $("review-kv");
  kv.replaceChildren();
  const row = (k: string, v: string, wrap = false) => {
    const dt = document.createElement("dt");
    dt.textContent = k;
    const dd = document.createElement("dd");
    dd.textContent = v;
    if (wrap) dd.style.textAlign = "left";
    kv.append(dt, dd);
  };
  for (const pay of p.payments) {
    row("To", chunk(pay.recipient), true);
    row("Amount", `${formatZec(pay.amountZat, ticker)} · ${pay.pool}${pay.hasMemo ? " · with memo" : ""}`);
  }
  for (const c of p.change) row("Change", `${formatZec(c.valueZat, ticker)} · ${c.pool}`);
  row("Fee", formatZec(p.feeZat, ticker));
  row("Total", formatZec(p.totalZat, ticker));
  $("review-note").textContent =
    `Check these against the device's review pages. The device receives a ${p.compact ? "compact" : "full"} PCZT of ` +
    `${p.signerViewBytes.toLocaleString("en-US")} bytes. Sign soon: a built transaction expires about 40 blocks after ${p.targetHeight}.`;
}

$<HTMLFormElement>("send-form").addEventListener("submit", async (ev) => {
  ev.preventDefault();
  const err = $("send-err");
  err.textContent = "";
  const to = $<HTMLTextAreaElement>("send-to").value.trim();
  const amount = $<HTMLInputElement>("send-amount").value.trim();
  const memo = $<HTMLTextAreaElement>("send-memo").value;
  $("send-to").setAttribute("aria-invalid", String(to === ""));
  const badAmount = !/^\d*(\.\d{0,8})?$/.test(amount) || amount === "" || amount === ".";
  $("send-amount").setAttribute("aria-invalid", String(badAmount));
  if (to === "" || badAmount) {
    err.textContent = `Enter an address and an amount in ${ticker} (at most 8 decimals).`;
    return;
  }
  const btn = $<HTMLButtonElement>("review-btn");
  btn.disabled = true;
  try {
    proposal = await invoke<Proposal>("propose_send", { to, amount, memo: memo.trim() === "" ? null : memo });
    // A shielding or swap deposit under review shares the one pending slot; this replaced it.
    if (shieldProposal) {
      shieldProposal = null;
      showShieldStage("idle");
      void renderBalances();
    }
    dropSwapReview();
    renderReview(proposal);
    $("sign-status").textContent = "";
    $("sign-err").textContent = "";
    $<HTMLProgressElement>("sign-bar").hidden = true;
    armMainnetConfirm("send-main-confirm", "sign-btn");
    showSendStage("review");
  } catch (e) {
    err.textContent = errText(e);
  } finally {
    btn.disabled = false;
  }
});

$("cancel-btn").addEventListener("click", () => {
  proposal = null;
  showSendStage("form");
});

function isMainnet(): boolean {
  return profile?.network === "main";
}

/** On mainnet the sign button waits for the real-funds tick, cleared for each new review. */
function armMainnetConfirm(box: string, sign: string): void {
  $<HTMLInputElement>(box).checked = false;
  $<HTMLButtonElement>(sign).disabled = isMainnet();
}

for (const [box, sign] of [
  ["send-main-confirm", "sign-btn"],
  ["shield-main-confirm", "shield-sign-btn"],
] as const) {
  $(box).addEventListener("change", () => {
    $<HTMLButtonElement>(sign).disabled = isMainnet() && !$<HTMLInputElement>(box).checked;
  });
}

/** The elements of one review: the send's or the shielding's. */
interface SignUi {
  sign: string;
  cancel: string;
  bar: string;
  status: string;
  err: string;
  /** The mainnet real-funds checkbox. */
  confirm: string;
}

/**
 * Signs a reviewed proposal on the device, then proves and broadcasts it
 * (the backend does both in `sign_and_send`). Returns null on failure,
 * with the reason shown.
 */
async function signOnDevice(p: Proposal, ui: SignUi): Promise<Sent | null> {
  $(ui.err).textContent = "";
  if (!paired) {
    $(ui.err).textContent = "Connect the LeekWallet first (above).";
    return null;
  }
  // Mainnet: the extra step before the device. The backend refuses the
  // call without it too.
  const confirmMainnet = $<HTMLInputElement>(ui.confirm).checked;
  if (isMainnet() && !confirmMainnet) {
    $(ui.err).textContent = "This moves real ZEC on mainnet. Tick the confirmation above first.";
    return null;
  }
  const bar = $<HTMLProgressElement>(ui.bar);
  bar.value = 0;
  uploadBar = bar;
  $<HTMLButtonElement>(ui.cancel).disabled = true;
  setConn("connecting", "Signing on LeekWallet…");
  try {
    const sent = await deviceFlow(ui.status, $<HTMLButtonElement>(ui.sign), () =>
      invoke<Sent>("sign_and_send", { proposalId: p.id, confirmMainnet }),
    );
    setConn("connected", p.kind === "shield" ? "Shielded" : "Sent");
    return sent;
  } catch (e) {
    $(ui.status).textContent = "";
    $(ui.err).textContent = errText(e);
    setConn("error", p.kind === "shield" ? "Not shielded" : "Not sent");
    await afterDeviceError();
    return null;
  } finally {
    uploadBar = null;
    bar.hidden = true;
    $<HTMLButtonElement>(ui.cancel).disabled = false;
  }
}

$("sign-btn").addEventListener("click", async () => {
  if (!proposal) return;
  const sent = await signOnDevice(proposal, {
    sign: "sign-btn",
    cancel: "cancel-btn",
    bar: "sign-bar",
    status: "sign-status",
    err: "sign-err",
    confirm: "send-main-confirm",
  });
  if (!sent) return;
  proposal = null;
  const txid = $("txid");
  txid.textContent = sent.txid;
  txid.dataset["raw"] = sent.txid;
  $<HTMLFormElement>("send-form").reset();
  showRecipientName();
  showSendStage("sent");
  await afterTransaction();
});

/** A transaction was stored and broadcast: what it changed. */
async function afterTransaction(): Promise<void> {
  await Promise.all([renderBalances(), renderHistory()]);
}

// ------------------------------------------------------------------- shield

let shieldProposal: Proposal | null = null;

function showShieldStage(stage: "idle" | "review" | "done"): void {
  $("shield-review").hidden = stage !== "review";
  $("shielded").hidden = stage !== "done";
  $("shield-row").hidden = stage !== "idle";
}

$("shield-btn").addEventListener("click", async () => {
  const btn = $<HTMLButtonElement>("shield-btn");
  const err = $("shield-err");
  err.textContent = "";
  btn.disabled = true;
  try {
    const p = await invoke<Proposal>("propose_shield");
    shieldProposal = p;
    // A send under review shares the one pending slot; this replaced it.
    if (proposal) {
      proposal = null;
      showSendStage("form");
    }
    dropSwapReview();
    const kv = $("shield-kv");
    kv.replaceChildren();
    const row = (k: string, v: string) => {
      const dt = document.createElement("dt");
      dt.textContent = k;
      const dd = document.createElement("dd");
      dd.textContent = v;
      kv.append(dt, dd);
    };
    const shielded = p.change.reduce((s, c) => s + c.valueZat, 0);
    const pools = [...new Set(p.change.map((c) => c.pool))].join(", ");
    row("From", "Your transparent funds");
    row("Shield", formatZec(shielded, ticker));
    row("Into", `${pools} (this wallet)`);
    row("Fee", formatZec(p.feeZat, ticker));
    row("Total", formatZec(shielded + p.feeZat, ticker));
    $("shield-note").textContent =
      `Nothing leaves the wallet but the fee. The device shows each transparent coin it signs, then "Shield … into this wallet". ` +
      `It receives a compact PCZT of ${p.signerViewBytes.toLocaleString("en-US")} bytes. Sign soon: the transaction expires about 40 blocks after ${p.targetHeight}.`;
    $("shield-status").textContent = "";
    $("shield-sign-err").textContent = "";
    armMainnetConfirm("shield-main-confirm", "shield-sign-btn");
    showShieldStage("review");
  } catch (e) {
    // E.g. the fee would take the whole transparent balance.
    err.textContent = errText(e);
  } finally {
    btn.disabled = false;
  }
});

$("shield-cancel-btn").addEventListener("click", () => {
  shieldProposal = null;
  showShieldStage("idle");
  void renderBalances();
});

$("shield-sign-btn").addEventListener("click", async () => {
  if (!shieldProposal) return;
  const sent = await signOnDevice(shieldProposal, {
    sign: "shield-sign-btn",
    cancel: "shield-cancel-btn",
    bar: "shield-bar",
    status: "shield-status",
    err: "shield-sign-err",
    confirm: "shield-main-confirm",
  });
  if (!sent) return;
  shieldProposal = null;
  const txid = $("shield-txid");
  txid.textContent = sent.txid;
  txid.dataset["raw"] = sent.txid;
  showShieldStage("done");
  await afterTransaction();
});

$("copy-shield-txid").addEventListener("click", () => void copyFrom($("copy-shield-txid"), $("shield-txid")));

$("shield-done-btn").addEventListener("click", () => {
  showShieldStage("idle");
  void renderBalances();
});

$("new-send-btn").addEventListener("click", () => showSendStage("form"));

$("copy-txid").addEventListener("click", async () => {
  try {
    await navigator.clipboard.writeText($("txid").dataset["raw"] ?? "");
    $("copy-txid").textContent = "Copied";
    setTimeout(() => ($("copy-txid").textContent = "Copy"), 1500);
  } catch {
    /* the txid is on screen to select by hand */
  }
});

// ------------------------------------------------------------------ history

const KIND_LABEL: Record<Tx["kind"], string> = {
  received: "Received",
  sent: "Sent",
  self: "Sent to yourself",
  shielded: "Shielded",
  internal: "Moved between pools",
};

function el<K extends keyof HTMLElementTagNameMap>(tag: K, cls: string, text?: string): HTMLElementTagNameMap[K] {
  const e = document.createElement(tag);
  e.className = cls;
  if (text !== undefined) e.textContent = text;
  return e;
}

function txWhen(t: Tx): string {
  if (t.minedHeight === null) {
    if (t.expired) return "Not mined, expired: it will not be";
    return t.expiryHeight !== null ? `Waiting to be mined (expires at block ${t.expiryHeight})` : "Waiting to be mined";
  }
  const at = `block ${t.minedHeight.toLocaleString("en-US")}`;
  if (t.blockTime === null) return at;
  const d = new Date(t.blockTime * 1000);
  return `${d.toLocaleString(undefined, { dateStyle: "medium", timeStyle: "short" })} · ${at}`;
}

async function renderHistory(): Promise<void> {
  const list = $("history");
  const err = $("history-err");
  let txs: Tx[];
  try {
    txs = await invoke<Tx[]>("history");
    err.textContent = "";
  } catch (e) {
    err.textContent = errText(e);
    return;
  }
  $("history-empty").hidden = txs.length > 0;
  const names = new Map(contactList.map((c) => [c.address, c.name]));
  list.replaceChildren(
    ...txs.map((t) => {
      const li = document.createElement("li");
      const head = el("div", "tx__head");
      head.append(el("span", "tx__kind", KIND_LABEL[t.kind]), el("span", "tx__when", txWhen(t)));
      const incoming = t.kind === "received";
      // A send to yourself moved nothing out: its amount is the balance
      // change, which is the fee, so it is not repeated as a fee line.
      const toSelf = t.kind === "self";
      const sign = incoming ? "+" : t.kind === "sent" || toSelf ? "−" : "";
      const amount = el("span", "tx__amount", `${sign}${formatZec(t.amountZat, ticker)}`);
      amount.dataset["sign"] = incoming ? "in" : "out";
      head.append(amount);
      li.append(head);
      if (toSelf) li.append(el("div", "tx__fee", "The fee: nothing left the wallet"));
      else if (t.feeZat !== null && !incoming) li.append(el("div", "tx__fee", `Fee ${formatZec(t.feeZat, ticker)}`));
      for (const r of t.recipients) {
        const who = names.get(r.address);
        li.append(el("div", "tx__to", `To ${who ? `${who} · ` : ""}${chunk(r.address)}`));
      }
      for (const m of t.memos) li.append(el("p", "tx__memo", m));
      const id = el("div", "tx__id");
      const code = el("code", "", t.txid);
      code.dataset["raw"] = t.txid;
      const copy = el("button", "", "Copy");
      copy.type = "button";
      copy.title = "Copy the transaction id";
      copy.addEventListener("click", () => void copyFrom(copy, code));
      id.append(code, copy);
      li.append(id);
      return li;
    }),
  );
}

$("history-refresh").addEventListener("click", () => void renderHistory());

// ----------------------------------------------------------------- contacts

let contactList: Contact[] = [];
/** The contact being edited, by its name; null when adding. */
let editing: string | null = null;

async function loadContacts(): Promise<void> {
  try {
    contactList = await invoke<Contact[]>("contacts_list");
    $("contact-err").textContent = "";
  } catch (e) {
    contactList = [];
    $("contact-err").textContent = errText(e);
  }
  renderContacts();
}

function renderContacts(): void {
  const list = $("contacts");
  list.replaceChildren(
    ...contactList.map((c) => {
      const li = document.createElement("li");
      const pay = el("button", "", "Pay");
      pay.type = "button";
      pay.hidden = $("send-panel").hidden;
      pay.addEventListener("click", () => pickContact(c));
      const edit = el("button", "", "Edit");
      edit.type = "button";
      edit.addEventListener("click", () => startEdit(c));
      // Two presses, so a slip does not lose a contact: the webview has no
      // confirm dialog on every platform.
      const del = el("button", "", "Delete");
      del.type = "button";
      let armed = 0;
      del.addEventListener("click", () => {
        if (armed) {
          clearTimeout(armed);
          void removeContact(c);
          return;
        }
        del.textContent = "Really delete?";
        armed = window.setTimeout(() => {
          armed = 0;
          del.textContent = "Delete";
        }, 3000);
      });
      li.append(el("span", "contact__name", c.name), el("span", "statusbar__spacer"), pay, edit, del, el("span", "contact__addr", chunk(c.address)));
      return li;
    }),
  );
  // The send form's picker.
  const sel = $<HTMLSelectElement>("send-contact");
  const keep = sel.value;
  sel.replaceChildren(sel.options[0] ?? new Option("Type an address below, or pick a contact", ""));
  for (const c of contactList) sel.append(new Option(c.name, c.address));
  sel.value = contactList.some((c) => c.address === keep) ? keep : "";
  $("send-contact-field").hidden = contactList.length === 0;
  showRecipientName();
}

/** Fills the send form with a contact's address. */
function pickContact(c: Contact): void {
  showSendStage("form");
  $<HTMLTextAreaElement>("send-to").value = c.address;
  $<HTMLSelectElement>("send-contact").value = c.address;
  showRecipientName();
  $("send-panel").scrollIntoView({ behavior: "smooth", block: "start" });
  $<HTMLInputElement>("send-amount").focus();
}

/** Names the recipient when the address typed is a contact's. */
function showRecipientName(): void {
  const to = $<HTMLTextAreaElement>("send-to").value.trim();
  const c = contactList.find((x) => x.address === to);
  $("send-to-name").textContent = c ? `Contact: ${c.name}` : "";
  $<HTMLSelectElement>("send-contact").value = c ? c.address : "";
}

$("send-contact").addEventListener("change", () => {
  const addr = $<HTMLSelectElement>("send-contact").value;
  if (addr !== "") $<HTMLTextAreaElement>("send-to").value = addr;
  showRecipientName();
});
$("send-to").addEventListener("input", showRecipientName);

function startEdit(c: Contact): void {
  editing = c.name;
  $<HTMLInputElement>("contact-name").value = c.name;
  $<HTMLTextAreaElement>("contact-address").value = c.address;
  $("contact-save").textContent = "Save contact";
  $("contact-cancel").hidden = false;
  $("contact-err").textContent = "";
  $<HTMLInputElement>("contact-name").focus();
}

function stopEdit(): void {
  editing = null;
  $<HTMLFormElement>("contact-form").reset();
  $("contact-save").textContent = "Add contact";
  $("contact-cancel").hidden = true;
  $("contact-name").setAttribute("aria-invalid", "false");
  $("contact-address").setAttribute("aria-invalid", "false");
}

$("contact-cancel").addEventListener("click", () => {
  stopEdit();
  $("contact-err").textContent = "";
});

$<HTMLFormElement>("contact-form").addEventListener("submit", async (ev) => {
  ev.preventDefault();
  const name = $<HTMLInputElement>("contact-name").value.trim();
  const address = $<HTMLTextAreaElement>("contact-address").value.trim();
  const err = $("contact-err");
  $("contact-name").setAttribute("aria-invalid", String(name === ""));
  $("contact-address").setAttribute("aria-invalid", String(address === ""));
  if (name === "" || address === "") {
    err.textContent = "Enter a name and an address.";
    return;
  }
  const btn = $<HTMLButtonElement>("contact-save");
  btn.disabled = true;
  try {
    contactList = await invoke<Contact[]>("contact_save", { name, address, replacing: editing });
    err.textContent = "";
    stopEdit();
    renderContacts();
    void renderHistory();
  } catch (e) {
    err.textContent = errText(e);
  } finally {
    btn.disabled = false;
  }
});

async function removeContact(c: Contact): Promise<void> {
  try {
    contactList = await invoke<Contact[]>("contact_remove", { name: c.name });
    if (editing === c.name) stopEdit();
    renderContacts();
    void renderHistory();
  } catch (e) {
    $("contact-err").textContent = errText(e);
  }
}

// ------------------------------------------------------------------ network

let network: NetworkSettings | null = null;

function renderNetwork(n: NetworkSettings): void {
  network = n;
  $<HTMLInputElement>(n.network === "main" ? "net-main" : "net-test").checked = true;
  $("net-confirm").hidden = true;
  $<HTMLInputElement>("server-input").value = n.server;
  $("server-label").textContent = `lightwalletd server for ${n.network === "main" ? "mainnet" : "testnet"}`;
  $("network-summary").textContent = `Network: ${n.network === "main" ? "Mainnet" : "Testnet"} · ${n.server}`;
  let note = `Default: ${n.defaultServer}. Each network keeps its own wallet and contacts in the app data folder.`;
  if (n.fromEnvironment) note += " This run's network or server comes from LEEKZ_NETWORK / LEEKZ_SERVER.";
  $("net-note").textContent = note;
}

async function loadNetwork(): Promise<void> {
  try {
    renderNetwork(await invoke<NetworkSettings>("network_settings"));
  } catch (e) {
    $("net-err").textContent = errText(e);
  }
}

/** Forgets everything on screen that belonged to the other network's wallet. */
function resetForNetwork(): void {
  paired = null;
  proposal = null;
  shieldProposal = null;
  rotated = null;
  contactList = [];
  $("pairing").hidden = true;
  for (const id of ["device-status", "device-err", "dev-import-err", "import-err", "sync-err", "shield-err",
    "verify-status", "verify-err", "send-err", "history-err", "contact-err"]) {
    $(id).textContent = "";
  }
  for (const id of ["send-form", "import-form", "device-import-form", "contact-form"]) $<HTMLFormElement>(id).reset();
  showSendStage("form");
  showShieldStage("idle");
  resetSwaps();
  $<HTMLProgressElement>("sync-bar").value = 0;
}

async function switchNetwork(to: "test" | "main"): Promise<void> {
  $("net-err").textContent = "";
  const radios = document.querySelectorAll<HTMLInputElement>('input[name="net"]');
  radios.forEach((r) => (r.disabled = true));
  try {
    const n = await invoke<NetworkSettings>("set_network", { network: to });
    resetForNetwork();
    renderNetwork(n);
    await refresh();
  } catch (e) {
    // E.g. a sync or a device call is running: stay where we are.
    $("net-err").textContent = errText(e);
    if (network) renderNetwork(network);
  } finally {
    radios.forEach((r) => (r.disabled = false));
  }
}

$("net-test").addEventListener("change", () => {
  if (network?.network !== "test") void switchNetwork("test");
});

// Mainnet takes one more step: the switch opens another wallet, for real funds.
$("net-main").addEventListener("change", () => {
  if (network?.network === "main") return;
  $("net-confirm").hidden = false;
});
$("net-confirm-btn").addEventListener("click", () => void switchNetwork("main"));
$("net-cancel-btn").addEventListener("click", () => {
  if (network) renderNetwork(network);
});

async function saveServer(server: string | null): Promise<void> {
  $("net-err").textContent = "";
  try {
    renderNetwork(await invoke<NetworkSettings>("set_server", { server }));
    await refresh();
  } catch (e) {
    $("net-err").textContent = errText(e);
  }
}

$("server-save").addEventListener("click", () => void saveServer($<HTMLInputElement>("server-input").value.trim()));
$("server-default").addEventListener("click", () => void saveServer(null));

// -------------------------------------------------------------------- swaps

// NEAR Intents through 1Click (backend: src-tauri/src/near.rs). The backend
// asks for quotes, keeps every swap with a deposit address in its own file,
// builds the ZEC deposit as an ordinary send, and asks 1Click for the
// status; this screen only draws and polls.

interface SwapToken {
  assetId: string;
  decimals: number;
  blockchain: string;
  symbol: string;
  price: number | null;
}

interface SwapInfo {
  network: string;
  authenticated: boolean;
  apiKeyEnv: string;
  explorer: string;
  defaultSlippageBps: number;
  maxSlippageBps: number;
}

type SwapState =
  | "quoted" | "expired" | "deposited" | "depositSeen" | "incomplete" | "processing"
  | "success" | "refunded" | "failed";

interface SwapAsset {
  assetId: string;
  symbol: string;
  blockchain: string;
  decimals: number;
}

interface SwapTxLink {
  hash: string;
  explorerUrl: string;
}

interface Swap {
  id: string;
  dry: boolean;
  createdAt: string;
  direction: "fromZec" | "toZec";
  origin: SwapAsset;
  destination: SwapAsset;
  amountIn: string;
  amountInFormatted: string;
  amountInUsd: string | null;
  minAmountInFormatted: string | null;
  amountOutFormatted: string;
  amountOutUsd: string | null;
  minAmountOutFormatted: string;
  slippageBps: number;
  deadline: string | null;
  timeEstimateSecs: number | null;
  refundFeeFormatted: string | null;
  depositAddress: string | null;
  depositMemo: string | null;
  depositTransparent: boolean;
  recipient: string;
  refundTo: string;
  confidentiality: string;
  authenticated: boolean;
  proposalId: string | null;
  depositTxid: string | null;
  depositSubmitted: boolean;
  state: SwapState;
  apiStatus: string | null;
  updatedAt: string | null;
  checkedAt: string | null;
  details: {
    amountInFormatted: string | null;
    amountOutFormatted: string | null;
    depositedAmountFormatted: string | null;
    refundedAmountFormatted: string | null;
    refundReason: string | null;
    originChainTxHashes: SwapTxLink[];
    destinationChainTxHashes: SwapTxLink[];
  } | null;
  lastError: string | null;
}

const FINAL_SWAP: SwapState[] = ["success", "refunded", "failed"];
const POLL_MS = 20_000;

let swapInfo: SwapInfo | null = null;
let swapTokens: SwapToken[] = [];
let swaps: Swap[] = [];
/** The quote on screen (dry or real). */
let swapQuote: Swap | null = null;
/** The deposit under review, and the swap it pays. */
let swapProposal: Proposal | null = null;
let swapPaying: Swap | null = null;
let pollTimer = 0;

function swapDirection(): "fromZec" | "toZec" {
  return $<HTMLInputElement>("swap-to").checked ? "toZec" : "fromZec";
}

function selectedToken(): SwapToken | null {
  const id = $<HTMLSelectElement>("swap-asset").value;
  return swapTokens.find((t) => t.assetId === id) ?? null;
}

const chainName = (c: string): string => c.toUpperCase();
const assetName = (a: { symbol: string; blockchain: string }): string =>
  a.blockchain === "zec" ? a.symbol : `${a.symbol} on ${chainName(a.blockchain)}`;

function when(iso: string | null): string {
  if (!iso) return "—";
  const d = new Date(iso);
  return Number.isNaN(d.getTime()) ? iso : d.toLocaleString(undefined, { dateStyle: "medium", timeStyle: "short" });
}

function usd(v: string | null): string {
  if (!v) return "";
  const n = Number(v);
  return Number.isFinite(n) ? ` (≈ $${n.toFixed(2)})` : "";
}

/** The labels and notes of the form for the direction and asset chosen. */
function renderSwapForm(): void {
  const dir = swapDirection();
  const t = selectedToken();
  const other = t ? assetName(t) : "the other asset";
  const main = isMainnet();
  $("swap-asset-label").textContent = dir === "fromZec" ? "Receive" : "Pay with";
  $("swap-amount-label").textContent = dir === "fromZec" ? "Amount to send (ZEC)" : `Amount to pay (${other})`;
  $("swap-addr-label").textContent =
    dir === "fromZec"
      ? `Recipient's address on ${t ? chainName(t.blockchain) : "the destination chain"}`
      : `Your refund address on ${t ? chainName(t.blockchain) : "the paying chain"} (if the swap fails, 1Click refunds there)`;
  const auth = swapInfo?.authenticated ?? false;
  const conf = $<HTMLInputElement>("swap-confidential");
  conf.disabled = !auth;
  if (!auth) conf.checked = false;
  let privacy =
    dir === "fromZec"
      ? "Refunds come back to a fresh shielded (Orchard) address of this wallet. The deposit address 1Click gives is " +
        "usually transparent: the deposit and its amount are public on the Zcash chain (the rest of your wallet is not)."
      : "The ZEC arrives at a fresh shielded (Orchard) address of this wallet, so the payout is private on the Zcash chain.";
  if (!auth) privacy += ` No 1Click API key is set (${swapInfo?.apiKeyEnv ?? "LEEKZ_1CLICK_API_KEY"}): 1Click adds a 0.25% fee.`;
  $("swap-privacy").textContent = privacy;
  $("swap-testnote").hidden = main;
  $("swap-quote-btn").hidden = !main;
}

async function loadSwapScreen(): Promise<void> {
  try {
    swapInfo = await invoke<SwapInfo>("swap_info");
    if ($<HTMLInputElement>("swap-slippage").value === "") {
      $<HTMLInputElement>("swap-slippage").value = String(swapInfo.defaultSlippageBps / 100);
    }
  } catch (e) {
    $("swap-err").textContent = errText(e);
  }
  renderSwapForm();
  await Promise.all([loadSwapTokens(), loadSwaps()]);
}

async function loadSwapTokens(): Promise<void> {
  const sel = $<HTMLSelectElement>("swap-asset");
  try {
    swapTokens = (await invoke<SwapToken[]>("swap_tokens")).filter((t) => t.blockchain !== "zec");
  } catch (e) {
    sel.replaceChildren(new Option("The asset list could not be loaded", ""));
    $("swap-err").textContent = errText(e);
    return;
  }
  const keep = sel.value;
  sel.replaceChildren(...swapTokens.map((t) => new Option(`${t.symbol} · ${chainName(t.blockchain)}`, t.assetId)));
  const usdcSol = swapTokens.find((t) => t.symbol === "USDC" && t.blockchain === "sol");
  sel.value = swapTokens.some((t) => t.assetId === keep) ? keep : (usdcSol?.assetId ?? swapTokens[0]?.assetId ?? "");
  renderSwapForm();
}

async function loadSwaps(): Promise<void> {
  try {
    swaps = await invoke<Swap[]>("swap_list");
    $("swap-list-err").textContent = "";
  } catch (e) {
    swaps = [];
    $("swap-list-err").textContent = errText(e);
  }
  renderSwaps();
  schedulePoll();
}

function showSwapStage(stage: "form" | "quote" | "review"): void {
  $("swap-form").hidden = stage !== "form";
  $("swap-quote").hidden = stage !== "quote";
  $("swap-review").hidden = stage !== "review";
}

/** The pending slot was taken by a send or a shielding: the deposit review is gone. */
function dropSwapReview(): void {
  if (!swapProposal) return;
  swapProposal = null;
  swapPaying = null;
  showSwapStage("form");
}

function resetSwaps(): void {
  window.clearTimeout(pollTimer);
  swaps = [];
  swapQuote = null;
  swapProposal = null;
  swapPaying = null;
  for (const id of ["swap-err", "swap-quote-err", "swap-sign-err", "swap-sign-status", "swap-list-err"]) $(id).textContent = "";
  $<HTMLTextAreaElement>("swap-addr").value = "";
  $<HTMLInputElement>("swap-amount").value = "";
  showSwapStage("form");
  renderSwaps();
}

for (const id of ["swap-from", "swap-to", "swap-asset"]) $(id).addEventListener("change", renderSwapForm);

async function requestSwapQuote(dry: boolean): Promise<void> {
  const err = $("swap-err");
  err.textContent = "";
  const t = selectedToken();
  const amount = $<HTMLInputElement>("swap-amount").value.trim();
  const addr = $<HTMLTextAreaElement>("swap-addr").value.trim();
  const slip = Number($<HTMLInputElement>("swap-slippage").value.trim());
  const maxSlip = (swapInfo?.maxSlippageBps ?? 1000) / 100;
  const badAmount = !/^\d*(\.\d+)?$/.test(amount) || amount === "" || amount === "." || Number(amount) <= 0;
  $("swap-amount").setAttribute("aria-invalid", String(badAmount));
  $("swap-addr").setAttribute("aria-invalid", String(addr === ""));
  if (!t) {
    err.textContent = "Choose an asset.";
    return;
  }
  if (badAmount || addr === "") {
    err.textContent = "Enter an amount and an address.";
    return;
  }
  if (!Number.isFinite(slip) || slip <= 0 || slip > maxSlip) {
    err.textContent = `Slippage between 0.01% and ${maxSlip}%.`;
    return;
  }
  const direction = swapDirection();
  const params = {
    direction,
    asset: t.assetId,
    amount,
    recipient: direction === "fromZec" ? addr : null,
    refundTo: direction === "toZec" ? addr : null,
    slippageBps: Math.round(slip * 100),
    dry,
    confidential: $<HTMLInputElement>("swap-confidential").checked,
  };
  const btns = [$<HTMLButtonElement>("swap-quote-btn"), $<HTMLButtonElement>("swap-preview-btn")];
  btns.forEach((b) => (b.disabled = true));
  try {
    swapQuote = await invoke<Swap>("swap_quote", { params });
    renderSwapQuote(swapQuote);
    showSwapStage("quote");
    if (!swapQuote.dry) await loadSwaps();
  } catch (e) {
    err.textContent = errText(e);
  } finally {
    btns.forEach((b) => (b.disabled = false));
  }
}

$<HTMLFormElement>("swap-form").addEventListener("submit", (ev) => {
  ev.preventDefault();
  if (isMainnet()) void requestSwapQuote(false);
});
$("swap-preview-btn").addEventListener("click", () => void requestSwapQuote(true));

function kvRows(kv: HTMLElement, rows: [string, string][]): void {
  kv.replaceChildren();
  for (const [k, v] of rows) {
    const dt = document.createElement("dt");
    dt.textContent = k;
    const dd = document.createElement("dd");
    dd.textContent = v;
    kv.append(dt, dd);
  }
}

function renderSwapQuote(q: Swap): void {
  const inName = assetName(q.origin);
  const outName = assetName(q.destination);
  const rate = Number(q.amountOutFormatted) / Number(q.amountInFormatted);
  const rows: [string, string][] = [
    ["You pay", `${q.amountInFormatted} ${inName}${usd(q.amountInUsd)}`],
    ["You receive about", `${q.amountOutFormatted} ${outName}${usd(q.amountOutUsd)}`],
    ["At least", `${q.minAmountOutFormatted} ${outName} (slippage ${q.slippageBps / 100}%)`],
  ];
  if (Number.isFinite(rate)) rows.push(["Rate", `1 ${q.origin.symbol} ≈ ${rate.toPrecision(6)} ${q.destination.symbol}`]);
  if (q.minAmountInFormatted && q.direction === "toZec") rows.push(["Least deposit swapped", `${q.minAmountInFormatted} ${inName}`]);
  if (q.refundFeeFormatted) rows.push(["Refund fee (if refunded)", `${q.refundFeeFormatted} ${q.origin.symbol}`]);
  rows.push(["Fees", q.authenticated ? "1Click's, included in the amount received" : "1Click's plus 0.25% (no API key), included"]);
  if (q.timeEstimateSecs !== null) rows.push(["Takes about", `${Math.max(1, Math.round(q.timeEstimateSecs / 60))} min after the deposit confirms`]);
  if (!q.dry) rows.push(["Deposit by", when(q.deadline)]);
  rows.push([q.direction === "fromZec" ? "Recipient" : "Into (this wallet)", chunk(q.recipient)]);
  rows.push(["Refunds to", q.direction === "fromZec" ? `${chunk(q.refundTo)} (this wallet, shielded)` : chunk(q.refundTo)]);
  if (q.confidentiality !== "public") rows.push(["Confidentiality", q.confidentiality]);
  kvRows($("swap-quote-kv"), rows);
  $("swap-quote-h").textContent = q.dry ? "Price preview (dry run)" : "Quote";

  const box = $("swap-deposit-box");
  const dep = q.depositAddress;
  box.hidden = dep === null;
  if (dep !== null) {
    $("swap-deposit-label").textContent =
      q.direction === "fromZec"
        ? `Deposit address (Zcash${q.depositTransparent ? ", transparent: the deposit is public" : ""}). "Review the deposit" builds the send; the LeekWallet shows this address.`
        : `Pay ${q.amountInFormatted} ${inName} from your ${chainName(q.origin.blockchain)} wallet to this address, before ${when(q.deadline)}. Only ${inName}: anything else is lost.`;
    $("swap-deposit").textContent = dep;
    $("swap-deposit").dataset["raw"] = dep;
    void drawSwapQr(q.direction === "toZec" ? dep : null);
  }
  $("swap-review-btn").hidden = q.dry || q.direction !== "fromZec";
  $("swap-quote-note").textContent = q.dry
    ? "A preview only: no deposit address was made. " + (isMainnet() ? "Get a quote to swap." : "Swaps need Mainnet.")
    : q.direction === "fromZec"
      ? "1Click's quote, not checked by the LeekWallet. Nothing is sent until you sign the deposit on the device."
      : "The swap appears below; this app follows it until the ZEC arrives (sync to see it in the balance).";
  $("swap-quote-err").textContent = "";
}

async function drawSwapQr(text: string | null): Promise<void> {
  const img = $<HTMLImageElement>("swap-deposit-qr");
  img.hidden = true;
  if (text === null) return;
  try {
    const svg = await invoke<string>("qr_svg", { text });
    img.src = `data:image/svg+xml;charset=utf-8,${encodeURIComponent(svg)}`;
    img.hidden = false;
  } catch {
    /* the address is on screen */
  }
}

$("copy-swap-deposit").addEventListener("click", () => void copyFrom($("copy-swap-deposit"), $("swap-deposit")));
$("swap-back-btn").addEventListener("click", () => {
  swapQuote = null;
  showSwapStage("form");
});
$("swap-review-btn").addEventListener("click", () => {
  if (swapQuote) void reviewDeposit(swapQuote);
});

/** Builds the ZEC deposit of `s` and shows it for review. */
async function reviewDeposit(s: Swap): Promise<void> {
  const err = swapQuote && !$("swap-quote").hidden ? $("swap-quote-err") : $("swap-list-err");
  err.textContent = "";
  try {
    const p = await invoke<Proposal>("swap_execute", { id: s.id });
    // It took the one pending slot from a send or a shielding.
    if (proposal) {
      proposal = null;
      showSendStage("form");
    }
    if (shieldProposal) {
      shieldProposal = null;
      showShieldStage("idle");
      void renderBalances();
    }
    swapProposal = p;
    swapPaying = s;
    const pay = p.payments[0];
    const rows: [string, string][] = [
      ["Swap", `${s.amountInFormatted} ZEC → about ${s.amountOutFormatted} ${assetName(s.destination)}`],
      ["Deposit to", chunk(pay ? pay.recipient : s.depositAddress ?? "")],
      ["Amount", `${formatZec(pay ? pay.amountZat : 0, ticker)} · ${pay ? pay.pool : ""}`],
    ];
    for (const c of p.change) rows.push(["Change", `${formatZec(c.valueZat, ticker)} · ${c.pool}`]);
    rows.push(["Fee", formatZec(p.feeZat, ticker)]);
    rows.push(["Total", formatZec(p.totalZat, ticker)]);
    kvRows($("swap-review-kv"), rows);
    $("swap-review-note").textContent =
      `The device shows the deposit address${s.depositTransparent ? " (transparent, with its warning)" : ""} and the amount: ` +
      `compare them with the ones here. Deposit before ${when(s.deadline)}; ` +
      `a built transaction also expires about 40 blocks after ${p.targetHeight}.`;
    $("swap-sign-status").textContent = "";
    $("swap-sign-err").textContent = "";
    armMainnetConfirm("swap-main-confirm", "swap-sign-btn");
    showSwapStage("review");
    $("swap-panel").scrollIntoView({ behavior: "smooth", block: "start" });
  } catch (e) {
    err.textContent = errText(e);
    await loadSwaps();
  }
}

$("swap-main-confirm").addEventListener("change", () => {
  $<HTMLButtonElement>("swap-sign-btn").disabled = isMainnet() && !$<HTMLInputElement>("swap-main-confirm").checked;
});

$("swap-cancel-btn").addEventListener("click", () => {
  swapProposal = null;
  swapPaying = null;
  showSwapStage(swapQuote ? "quote" : "form");
});

$("swap-sign-btn").addEventListener("click", async () => {
  if (!swapProposal || !swapPaying) return;
  const paying = swapPaying;
  const sent = await signOnDevice(swapProposal, {
    sign: "swap-sign-btn",
    cancel: "swap-cancel-btn",
    bar: "swap-bar",
    status: "swap-sign-status",
    err: "swap-sign-err",
    confirm: "swap-main-confirm",
  });
  if (!sent) return;
  swapProposal = null;
  swapPaying = null;
  swapQuote = null;
  $<HTMLFormElement>("swap-form").reset();
  $<HTMLInputElement>("swap-slippage").value = String((swapInfo?.defaultSlippageBps ?? 100) / 100);
  renderSwapForm();
  showSwapStage("form");
  // The backend recorded the txid with the swap; tell 1Click at once.
  await loadSwaps();
  await checkSwap(paying.id);
  await afterTransaction();
});

const STEPS: { key: string; label: string }[] = [
  { key: "quoted", label: "Quoted" },
  { key: "deposit", label: "Deposit sent" },
  { key: "seen", label: "Deposit seen" },
  { key: "processing", label: "Swapping" },
  { key: "done", label: "Done" },
];

function stepIndex(s: SwapState): number {
  switch (s) {
    case "quoted":
    case "expired":
      return 0;
    case "deposited":
      return 1;
    case "depositSeen":
    case "incomplete":
      return 2;
    case "processing":
      return 3;
    default:
      return 4;
  }
}

const STATE_TEXT: Record<SwapState, string> = {
  quoted: "Waiting for the deposit",
  expired: "Expired: no deposit before the deadline (a late one is refunded)",
  deposited: "Deposit broadcast; waiting for 1Click to see it",
  depositSeen: "1Click saw the deposit",
  incomplete: "Less than the quote needs arrived; 1Click refunds it after the deadline unless more comes",
  processing: "Swapping",
  success: "Done",
  refunded: "Refunded",
  failed: "Failed",
};

function timeline(s: Swap): HTMLElement {
  const ol = el("ol", "timeline");
  const now = stepIndex(s.state);
  STEPS.forEach((st, i) => {
    let label = st.label;
    if (i === 4 && s.state === "refunded") label = "Refunded";
    if (i === 4 && s.state === "failed") label = "Failed";
    if (i === 0 && s.state === "expired") label = "Expired";
    const li = el("li", "", `${i > 0 ? "→ " : ""}${label}`);
    const bad = (i === 4 && (s.state === "refunded" || s.state === "failed")) || (i === 0 && s.state === "expired");
    li.dataset["step"] = bad ? "bad" : i < now ? "done" : i === now ? (s.state === "success" ? "done" : "now") : "todo";
    ol.append(li);
  });
  return ol;
}

function copyLine(cls: string, label: string, value: string, title: string): HTMLElement {
  const row = el("div", "tx__id");
  const code = el("code", cls, `${label} ${value}`);
  code.dataset["raw"] = value;
  const copy = el("button", "", "Copy");
  copy.type = "button";
  copy.title = title;
  copy.addEventListener("click", () => void copyFrom(copy, code));
  row.append(code, copy);
  return row;
}

function renderSwaps(): void {
  const list = $("swap-list");
  $("swap-empty").hidden = swaps.length > 0;
  list.replaceChildren(
    ...swaps.map((s) => {
      const li = document.createElement("li");
      const head = el("div", "tx__head");
      const kind = s.direction === "fromZec" ? `ZEC → ${assetName(s.destination)}` : `${assetName(s.origin)} → ZEC`;
      head.append(el("span", "tx__kind", kind), el("span", "tx__when", when(s.createdAt)));
      head.append(el("span", "tx__amount", `${s.amountInFormatted} ${s.origin.symbol} → ${s.details?.amountOutFormatted ?? `≈ ${s.amountOutFormatted}`} ${s.destination.symbol}`));
      li.append(head, timeline(s));
      li.append(el("div", "swap__line", STATE_TEXT[s.state] ?? s.state));
      if (s.state === "quoted" || s.state === "expired") li.append(el("div", "swap__line", `Deposit by ${when(s.deadline)}`));
      if (s.direction === "toZec" && (s.state === "quoted" || s.state === "incomplete")) {
        li.append(el("div", "swap__line", `Pay ${s.amountInFormatted} ${assetName(s.origin)} to the deposit address below, from your wallet on ${chainName(s.origin.blockchain)}.`));
      }
      const d = s.details;
      /* 1Click reports refundedAmountFormatted as "0" on every swap, refunded
       * or not: a non-empty string is not a refund. Only an amount above zero
       * is; until the swap ends, say where a refund would go. */
      if (d?.refundedAmountFormatted && Number(d.refundedAmountFormatted) > 0) {
        li.append(el("div", "swap__line", `Refunded ${d.refundedAmountFormatted} ${s.origin.symbol} to ${chunk(s.refundTo)}${d.refundReason ? ` (${d.refundReason})` : ""}`));
      } else if (s.refundTo && !FINAL_SWAP.includes(s.state)) {
        li.append(el("div", "swap__line", `A refund, if any, would go to ${chunk(s.refundTo)}`));
      }
      if (s.depositAddress) li.append(copyLine("", "Deposit", s.depositAddress, "Copy the deposit address (look it up on " + (swapInfo?.explorer ?? "explorer.near-intents.org") + ")"));
      if (s.depositTxid) li.append(copyLine("", "Deposit tx", s.depositTxid, "Copy the deposit transaction id"));
      for (const t of d?.destinationChainTxHashes ?? []) {
        li.append(copyLine("", "Payout", t.explorerUrl || t.hash, "Copy the payout transaction (explorer link)"));
      }
      if (s.lastError) li.append(el("div", "swap__line", `Last check: ${s.lastError}`));
      else if (s.checkedAt) li.append(el("div", "swap__line", `Checked ${when(s.checkedAt)}${s.apiStatus ? ` · 1Click: ${s.apiStatus}` : ""}`));

      const actions = el("div", "swap__actions");
      if (s.direction === "fromZec" && s.state === "quoted" && !s.depositTxid && profile?.hardware) {
        const pay = el("button", "primary", "Review the deposit");
        pay.type = "button";
        pay.addEventListener("click", () => void reviewDeposit(s));
        actions.append(pay);
      }
      if (!FINAL_SWAP.includes(s.state)) {
        const chk = el("button", "", "Check now");
        chk.type = "button";
        chk.addEventListener("click", () => void checkSwap(s.id));
        actions.append(chk);
      }
      const removable = FINAL_SWAP.includes(s.state) || s.state === "expired" ||
        (s.direction === "fromZec" && s.state === "quoted" && !s.depositTxid);
      if (removable) {
        const rm = el("button", "", "Remove");
        rm.type = "button";
        rm.title = "Forget this swap on this computer (it can no longer move funds)";
        rm.addEventListener("click", async () => {
          try {
            swaps = await invoke<Swap[]>("swap_forget", { id: s.id });
            renderSwaps();
          } catch (e) {
            $("swap-list-err").textContent = errText(e);
          }
        });
        actions.append(rm);
      }
      li.append(actions);
      return li;
    }),
  );
}

async function checkSwap(id: string): Promise<void> {
  try {
    const s = await invoke<Swap>("swap_status", { id });
    const i = swaps.findIndex((x) => x.id === id);
    const was = i >= 0 ? swaps[i] : undefined;
    if (i >= 0) swaps[i] = s;
    else swaps.unshift(s);
    renderSwaps();
    // ZEC arrived or came back: show it.
    if (was && was.state !== s.state && (s.state === "success" || s.state === "refunded")) {
      void renderBalances();
      void renderHistory();
    }
  } catch (e) {
    $("swap-list-err").textContent = errText(e);
  }
}

/** Follows every swap that is not final while the swap screen is shown on mainnet. */
function schedulePoll(): void {
  window.clearTimeout(pollTimer);
  const open = swaps.filter((s) => !FINAL_SWAP.includes(s.state));
  if (!isMainnet() || open.length === 0) return;
  pollTimer = window.setTimeout(async () => {
    if (!$("swap-panel").hidden) {
      for (const s of open) await checkSwap(s.id);
    }
    schedulePoll();
  }, POLL_MS);
}

$("swap-refresh").addEventListener("click", async () => {
  const btn = $<HTMLButtonElement>("swap-refresh");
  btn.disabled = true;
  $("swap-list-err").textContent = "";
  try {
    for (const s of swaps.filter((x) => !FINAL_SWAP.includes(x.state))) await checkSwap(s.id);
    schedulePoll();
  } finally {
    btn.disabled = false;
  }
});

// -------------------------------------------------------------------- theme

type Theme = "system" | "light" | "dark";
const THEME_KEY = "leekz.theme";
const THEME_ORDER: Theme[] = ["system", "light", "dark"];
const THEME_ICON: Record<Theme, string> = { system: "◐", light: "☀", dark: "☾" };

function applyTheme(theme: Theme): void {
  const root = document.documentElement;
  if (theme === "system") delete root.dataset["theme"];
  else root.dataset["theme"] = theme;
  const btn = $("theme");
  btn.textContent = THEME_ICON[theme];
  btn.setAttribute("aria-label", `Theme: ${theme}. Click to change.`);
  btn.title = `Theme: ${theme}`;
}

function currentTheme(): Theme {
  try {
    const t = localStorage.getItem(THEME_KEY);
    return t === "light" || t === "dark" || t === "system" ? t : "system";
  } catch {
    return "system";
  }
}

$("theme").addEventListener("click", () => {
  const next = THEME_ORDER[(THEME_ORDER.indexOf(currentTheme()) + 1) % THEME_ORDER.length] ?? "system";
  try {
    localStorage.setItem(THEME_KEY, next);
  } catch {
    /* not persisted; still applied */
  }
  applyTheme(next);
});

applyTheme(currentTheme());

void loadNetwork();
refresh().catch((e) => {
  setConn("error", errText(e));
});
void loadLinks();
