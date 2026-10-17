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
  await Promise.all([renderAddresses(), renderBalances()]);
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

async function renderAddresses(): Promise<void> {
  const a = await invoke<Addresses>("addresses");
  const ua = $("ua");
  ua.textContent = chunk(a.unified);
  ua.dataset["raw"] = a.unified;
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
    await renderBalances();
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

$("copy-ua").addEventListener("click", async () => {
  const raw = $("ua").dataset["raw"] ?? "";
  try {
    await navigator.clipboard.writeText(raw);
    $("copy-ua").textContent = "Copied";
    setTimeout(() => ($("copy-ua").textContent = "Copy"), 1500);
  } catch {
    // Clipboard denied: select the text so it can be copied by hand.
    const r = document.createRange();
    r.selectNodeContents($("ua"));
    getSelection()?.removeAllRanges();
    getSelection()?.addRange(r);
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

$("sign-btn").addEventListener("click", async () => {
  if (!proposal) return;
  $("sign-err").textContent = "";
  if (!paired) {
    $("sign-err").textContent = "Connect the LeekWallet first (above).";
    return;
  }
  const bar = $<HTMLProgressElement>("sign-bar");
  bar.value = 0;
  uploadBar = bar;
  $<HTMLButtonElement>("cancel-btn").disabled = true;
  setConn("connecting", "Signing on LeekWallet…");
  try {
    const sent = await deviceFlow("sign-status", $<HTMLButtonElement>("sign-btn"), () =>
      invoke<Sent>("sign_and_send", { proposalId: proposal?.id }),
    );
    proposal = null;
    const txid = $("txid");
    txid.textContent = sent.txid;
    txid.dataset["raw"] = sent.txid;
    $<HTMLFormElement>("send-form").reset();
    showSendStage("sent");
    setConn("connected", "Sent");
    await renderBalances();
  } catch (e) {
    $("sign-status").textContent = "";
    $("sign-err").textContent = errText(e);
    setConn("error", "Not sent");
    await afterDeviceError();
  } finally {
    uploadBar = null;
    bar.hidden = true;
    $<HTMLButtonElement>("cancel-btn").disabled = false;
  }
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
