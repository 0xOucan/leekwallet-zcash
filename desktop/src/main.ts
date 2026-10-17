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
  kind: "received" | "sent" | "shielded" | "internal";
  amountZat: number;
  deltaZat: number;
  /** Only when this wallet paid it. */
  feeZat: number | null;
  memos: string[];
  recipients: { address: string; valueZat: number }[];
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
  $("netnote").hidden = s.network === "main";

  // A watch-only account has no device; an empty wallet offers both ways in.
  $("device-panel").hidden = s.hasAccount && !s.hardware;
  $("import-panel").hidden = s.hasAccount;
  $("sync-panel").hidden = !s.hasAccount;
  $("balance-panel").hidden = !s.hasAccount;
  $("receive-panel").hidden = !s.hasAccount;
  $("send-panel").hidden = !(s.hasAccount && s.hardware);
  $("history-panel").hidden = !s.hasAccount;
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
  await Promise.all([renderAddresses(), renderBalances(), renderHistory()]);
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
    // A shielding under review shares the one pending slot; this replaced it.
    if (shieldProposal) {
      shieldProposal = null;
      showShieldStage("idle");
      void renderBalances();
    }
    renderReview(proposal);
    $("sign-status").textContent = "";
    $("sign-err").textContent = "";
    $<HTMLProgressElement>("sign-bar").hidden = true;
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

/** The elements of one review: the send's or the shielding's. */
interface SignUi {
  sign: string;
  cancel: string;
  bar: string;
  status: string;
  err: string;
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
  const bar = $<HTMLProgressElement>(ui.bar);
  bar.value = 0;
  uploadBar = bar;
  $<HTMLButtonElement>(ui.cancel).disabled = true;
  setConn("connecting", "Signing on LeekWallet…");
  try {
    const sent = await deviceFlow(ui.status, $<HTMLButtonElement>(ui.sign), () =>
      invoke<Sent>("sign_and_send", { proposalId: p.id }),
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
  });
  if (!sent) return;
  proposal = null;
  const txid = $("txid");
  txid.textContent = sent.txid;
  txid.dataset["raw"] = sent.txid;
  $<HTMLFormElement>("send-form").reset();
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
  list.replaceChildren(
    ...txs.map((t) => {
      const li = document.createElement("li");
      const head = el("div", "tx__head");
      head.append(el("span", "tx__kind", KIND_LABEL[t.kind]), el("span", "tx__when", txWhen(t)));
      const incoming = t.kind === "received";
      const amount = el("span", "tx__amount", `${incoming ? "+" : t.kind === "sent" ? "−" : ""}${formatZec(t.amountZat, ticker)}`);
      amount.dataset["sign"] = incoming ? "in" : "out";
      head.append(amount);
      li.append(head);
      if (t.feeZat !== null && !incoming) li.append(el("div", "tx__fee", `Fee ${formatZec(t.feeZat, ticker)}`));
      for (const r of t.recipients) {
        li.append(el("div", "tx__to", `To ${chunk(r.address)}`));
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

refresh().catch((e) => {
  setConn("error", errText(e));
});
void loadLinks();
