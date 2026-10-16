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

// ------------------------------------------------------------------- render

async function refresh(): Promise<void> {
  const s = await invoke<ProfileStatus>("profile_status");
  ticker = s.network === "main" ? "ZEC" : "TAZ";
  const net = $("network");
  net.textContent = s.network === "main" ? "mainnet" : "testnet";
  net.dataset["net"] = s.network;
  $("server").textContent = s.server;
  $("server").title = `lightwalletd server ${s.server}; wallet data in ${s.dataDir}`;
  $("netnote").hidden = s.network === "main";

  $("import-panel").hidden = s.hasAccount;
  $("sync-panel").hidden = !s.hasAccount;
  $("balance-panel").hidden = !s.hasAccount;
  $("receive-panel").hidden = !s.hasAccount;
  if (!s.hasAccount) {
    setConn("disconnected", "No account yet");
    return;
  }

  if (s.fullyScanned !== null && s.chainTip !== null) {
    showScanned(s.fullyScanned, s.chainTip, s.birthday ?? s.fullyScanned);
  } else {
    $("sync-status").textContent = `Not synced yet. Scanning starts at the birthday, block ${s.birthday ?? "?"}.`;
  }
  setConn("connected", "Watch-only account");
  await Promise.all([renderAddresses(), renderBalances()]);
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
