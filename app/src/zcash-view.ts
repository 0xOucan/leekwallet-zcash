/**
 * The Zcash section (index.html #zcashpanel): network switch, the board's
 * account, the address check, the birthday, and signing a PCZT file.
 *
 * Everything that touches the board goes through `deps.call()`, which is
 * main.ts's Client: the one serialised, encrypted session of the app, so
 * two requests can never interleave frames. The protocol itself is
 * packages/core/src/zcash.ts; this file is only the DOM around it.
 *
 * Board errors are shown as the board wrote them (zErrorText): a refusal
 * this app has never heard of, such as the board's network guard, reads
 * correctly without a release of this app.
 */

import {
  checkPczt, encodeZsig, sha256, toHex, zErrorText, zSetBirthday, zShowAddress, zSign,
  zsigName, MAX_BIRTHDAY, type ZAccount, type ZCall, type ZNetwork,
} from "../packages/core/src/zcash.ts";
import type { Invoke } from "./tauri-transport.ts";
import { readAccountRetrying } from "./zcash-auto.ts";
import { isWrongNetwork, wrongNetwork } from "./network-guard.ts";

export interface ZcashDeps {
  /** The paired client's call, or null when there is no encrypted session. */
  call(): ZCall | null;
  log(line: string): void;
  copy(text: string): Promise<string>;
  qr(text: string): SVGSVGElement;
  invoke(): Invoke | null;
}

const NETWORK_KEY = "leek.zcash.network";

const $ = <T extends HTMLElement>(id: string): T => {
  const el = document.getElementById(id);
  if (!el) throw new Error(`missing element #${id}`);
  return el as T;
};

/** The saved network; testnet when nothing (or nothing readable) was saved. */
export function savedNetwork(): ZNetwork {
  try {
    return localStorage.getItem(NETWORK_KEY) === "main" ? "main" : "test";
  } catch {
    return "test";
  }
}

export const networkLabel = (n: ZNetwork): string => (n === "main" ? "Mainnet" : "Testnet");

export interface ZcashView {
  /** Forget everything read from the board (lock, disconnect, new session). */
  reset(): void;
  readonly network: ZNetwork;
  readonly account: number;
  /** Subscribe to network or account changes (the wallet follows them). */
  onChange(f: () => void): void;
  /** The account last read from the board on the current network, if any. */
  readonly boardAccount: ZAccount | null;
  /** Read the board's account (the wallet's import uses it too). */
  readAccount(): Promise<ZAccount>;
  /**
   * The board's account changed: read, re-read under another seed (a
   * passphrase), or forgotten (null) on reset, network or account change.
   * The wallet follows it, because its seed fingerprint names the wallet.
   */
  onAccount(f: (a: ZAccount | null) => void): void;
  /** The home screen opened on an unlocked board: read its account. */
  opened(): void;
  busy<T>(what: string, f: () => Promise<T>): Promise<T>;
  /** A board operation is running (busy() would refuse another). */
  readonly working: boolean;
  /**
   * The board said which network it is on (getStatus `network`, newer
   * firmware): the app follows it, without the Mainnet question, since
   * the board's own setting is the user's choice already made. True when
   * the app switched.
   */
  followBoard(n: ZNetwork): boolean;
}

export function initZcash(deps: ZcashDeps): ZcashView {
  let network: ZNetwork = savedNetwork();
  let account = 0;
  let current: ZAccount | null = null;
  let pczt: { name: string; bytes: Uint8Array } | null = null;
  let working = false;
  const listeners: (() => void)[] = [];
  const accountListeners: ((a: ZAccount | null) => void)[] = [];
  const announceAccount = (a: ZAccount | null) => { for (const f of accountListeners) f(a); };

  const status = (text: string) => { $("zstatus").textContent = text; };

  function requireCall(): ZCall {
    const c = deps.call();
    if (!c) throw new Error("Connect and pair the board first (Link, above).");
    return c;
  }

  /** One board operation at a time, with every board button disabled meanwhile. */
  async function busy<T>(what: string, f: () => Promise<T>): Promise<T> {
    if (working) throw new Error("The board is busy with another request.");
    working = true;
    for (const b of Array.from(document.querySelectorAll<HTMLButtonElement>("#zcashpanel [data-board]"))) b.disabled = true;
    try {
      return await f();
    } catch (e) {
      deps.log(`zcash: ${what} failed: ${zErrorText(e)}`);
      /* The board is on the other network: say so with the way out, on
       * every tab, not only in this operation's status line. */
      if (isWrongNetwork(e)) showWrongNetwork(e);
      throw e;
    } finally {
      working = false;
      for (const b of Array.from(document.querySelectorAll<HTMLButtonElement>("#zcashpanel [data-board]"))) b.disabled = false;
      $<HTMLButtonElement>("zsign").disabled = pczt === null;
    }
  }

  function renderNetwork(): void {
    $("znettest").setAttribute("aria-pressed", String(network === "test"));
    $("znetmain").setAttribute("aria-pressed", String(network === "main"));
    $("zmainbanner").hidden = network !== "main";
  }

  function clearAccount(): void {
    const had = current !== null;
    current = null;
    if (had) announceAccount(null);
    $("zacct").hidden = true;
    $("zqr").hidden = true;
    $("zqr").replaceChildren();
    for (const id of ["zuaqr", "ztqr"]) {
      $(id).setAttribute("aria-expanded", "false");
      $(id).textContent = "Show QR code";
    }
    status("");
  }

  function clearSigned(): void {
    $("zsigned").hidden = true;
    $("zsignstatus").textContent = "";
    $("zsignbar").hidden = true;
  }

  function changed(): void {
    clearAccount();
    clearSigned();
    for (const f of listeners) f();
    /* Another network or account is another wallet: read it right away
     * when there is a board to ask, as the home screen does. */
    autoRead();
  }

  /** Read the account unless one is already on screen or the board is busy. */
  function autoRead(): void {
    if (current !== null || working || deps.call() === null) return;
    void busy("reading the account", readAccount).catch((e) => status(zErrorText(e)));
  }

  /** The board refused a request for this network: the notice and its button. */
  let wrongTo: ZNetwork | null = null;
  function showWrongNetwork(e: unknown): void {
    const w = wrongNetwork(network, (e as { message?: string }).message);
    wrongTo = w.board;
    $("zwrongnettext").textContent = w.text;
    $("zwrongnetgo").textContent = w.button;
    $("zwrongnet").hidden = false;
  }
  function hideWrongNetwork(): void {
    wrongTo = null;
    $("zwrongnet").hidden = true;
  }

  function setNetwork(n: ZNetwork): void {
    $("zmainask").hidden = true;
    hideWrongNetwork();
    if (n === network) return;
    network = n;
    try { localStorage.setItem(NETWORK_KEY, n); } catch { /* the choice still holds for this run */ }
    deps.log(`zcash: network ${networkLabel(n)}`);
    renderNetwork();
    changed();
  }

  function renderAccount(a: ZAccount): void {
    const before = current;
    current = a;
    $("zua").textContent = a.ua;
    $("ztaddr").textContent = a.tAddress;
    $("zufvk").textContent = a.ufvk;
    $("zfp").textContent = a.seedFingerprint;
    $("zbday").textContent = a.birthday === null ? "not set on the board" : String(a.birthday);
    $("zacct").hidden = false;
    if (before?.seedFingerprint !== a.seedFingerprint || before?.account !== a.account || before?.network !== a.network) {
      announceAccount(a);
    }
  }

  async function readAccount(): Promise<ZAccount> {
    const call = requireCall();
    const n = network;
    const i = account;
    status(`Reading account ${i} (${networkLabel(n)}) from the board…`);
    /* Right after the PIN the board can answer 0x0300 ("no wallet on the
     * device", "derivation failed") while its vault is still opening; a
     * read a moment later works (docs/JOURNAL.md, the 0x0300 retry). The
     * home screen reads exactly then, so retry once. */
    const a = await readAccountRetrying(call, i, n, {
      onRetry: () => status("The board is still opening its wallet; asking again…"),
    });
    /* The user may have switched while the board was deriving. */
    if (n !== network || i !== account) throw new Error("the network or account changed while reading");
    hideWrongNetwork();
    renderAccount(a);
    status(`Account ${a.account}, ${networkLabel(a.network)}. Verify the address on the board before you receive.`);
    deps.log(`zcash: read account ${a.account} on ${a.network}`);
    return a;
  }

  function toggleQr(button: string, text: () => string | undefined): void {
    const qr = $("zqr");
    const open = $(button).getAttribute("aria-expanded") === "true";
    for (const id of ["zuaqr", "ztqr"]) {
      $(id).setAttribute("aria-expanded", "false");
      $(id).textContent = "Show QR code";
    }
    const t = text();
    if (open || !t) {
      qr.hidden = true;
      qr.replaceChildren();
      return;
    }
    qr.replaceChildren(deps.qr(t));
    qr.hidden = false;
    $(button).setAttribute("aria-expanded", "true");
    $(button).textContent = "Hide QR code";
  }

  async function copy(text: string | undefined, what: string): Promise<void> {
    if (!text) return;
    const route = await deps.copy(text);
    status(route === "manual" ? `Copy the ${what} from the box below.` : `Copied the ${what}.`);
  }

  async function save(name: string, bytes: Uint8Array): Promise<string> {
    const invoke = deps.invoke();
    if (invoke) return invoke<string>("save_download", { name, bytes: Array.from(bytes) });
    /* A browser tab (the dev server): its own download. */
    const a = document.createElement("a");
    a.href = URL.createObjectURL(new Blob([bytes as Uint8Array<ArrayBuffer>], { type: "application/octet-stream" }));
    a.download = name;
    a.click();
    setTimeout(() => URL.revokeObjectURL(a.href), 10_000);
    return `${name} (your browser's downloads)`;
  }

  /* ----------------------------------------------------------- wiring */

  for (const b of ["zread", "zverify", "zbdayset"]) $(b).dataset["board"] = "1";

  $("znettest").addEventListener("click", () => setNetwork("test"));
  $("znetmain").addEventListener("click", () => {
    if (network === "main") return;
    $("zmainask").hidden = false;
    $("zmainno").focus();
  });
  $("zmainyes").addEventListener("click", () => setNetwork("main"));
  /* One tap: the app moves to the board's network, which reads the
   * account again (changed() -> autoRead) and opens that wallet. Mainnet
   * keeps its banner; the board's setting was the user's choice. */
  $("zwrongnetgo").addEventListener("click", () => {
    const to = wrongTo;
    if (!to) return;
    deps.log(`zcash: switching the app to ${networkLabel(to)}, the board's network`);
    setNetwork(to);
  });
  $("zmainno").addEventListener("click", () => { $("zmainask").hidden = true; });

  $<HTMLInputElement>("zaccount").addEventListener("change", (ev) => {
    const input = ev.target as HTMLInputElement;
    const n = Number(input.value.trim());
    if (!Number.isInteger(n) || n < 0 || n >= 0x80000000) {
      input.setAttribute("aria-invalid", "true");
      status("The account is a whole number from 0 (the first account).");
      return;
    }
    input.removeAttribute("aria-invalid");
    if (n === account) return;
    account = n;
    changed();
  });

  for (const tab of Array.from(document.querySelectorAll<HTMLButtonElement>("#zcashpanel [role=tab]"))) {
    tab.addEventListener("click", () => {
      for (const t of Array.from(document.querySelectorAll<HTMLButtonElement>("#zcashpanel [role=tab]"))) {
        const on = t === tab;
        t.setAttribute("aria-selected", String(on));
        const pane = document.getElementById(t.getAttribute("aria-controls") ?? "");
        if (pane) pane.hidden = !on;
      }
    });
  }

  $("zread").addEventListener("click", () => {
    void busy("reading the account", readAccount).catch((e) => status(zErrorText(e)));
  });

  $("zverify").addEventListener("click", () => {
    const a = current;
    if (!a) return;
    void busy("the address check", async () => {
      status("Compare the address on the board with the one here, then answer on the board.");
      await zShowAddress(requireCall(), a.account, a.network, a.ua);
      status("The board confirmed this address is its own, and you said it matches.");
      deps.log(`zcash: address of account ${a.account} confirmed on the board`);
    }).catch((e) => status(zErrorText(e)));
  });

  $("zuaqr").addEventListener("click", () => toggleQr("zuaqr", () => current?.ua));
  $("ztqr").addEventListener("click", () => toggleQr("ztqr", () => current?.tAddress));
  $("zuacopy").addEventListener("click", () => void copy(current?.ua, "unified address"));
  $("ztcopy").addEventListener("click", () => void copy(current?.tAddress, "transparent address"));
  $("zufvkcopy").addEventListener("click", () => void copy(current?.ufvk, "viewing key"));

  $("zbdayset").addEventListener("click", () => {
    const a = current;
    if (!a) return;
    const input = $<HTMLInputElement>("zbdayin");
    const h = Number(input.value.trim());
    if (!Number.isInteger(h) || h < 1 || h > MAX_BIRTHDAY) {
      input.setAttribute("aria-invalid", "true");
      status("The birthday is a block height, a whole number.");
      return;
    }
    input.removeAttribute("aria-invalid");
    void busy("setting the birthday", async () => {
      const stored = await zSetBirthday(requireCall(), a.account, a.network, h);
      current = { ...a, birthday: h };
      $("zbday").textContent = String(h);
      status(stored
        ? `Birthday ${h} recorded on the board for ${networkLabel(a.network)}.`
        : `Birthday ${h} held by the board until it locks (this seed is not stored in its flash).`);
    }).catch((e) => status(zErrorText(e)));
  });

  $<HTMLInputElement>("zpczt").addEventListener("change", (ev) => {
    const file = (ev.target as HTMLInputElement).files?.[0];
    pczt = null;
    clearSigned();
    $("zpcztinfo").hidden = true;
    $<HTMLButtonElement>("zsign").disabled = true;
    if (!file) return;
    void file.arrayBuffer().then((buf) => {
      const bytes = new Uint8Array(buf);
      try {
        checkPczt(bytes);
      } catch (e) {
        $("zsignstatus").textContent = (e as Error).message;
        return;
      }
      pczt = { name: file.name, bytes };
      $("zpcztname").textContent = file.name;
      $("zpcztsize").textContent = `${bytes.length.toLocaleString()} bytes`;
      $("zpcztsha").textContent = toHex(sha256(bytes));
      $("zpcztinfo").hidden = false;
      $<HTMLButtonElement>("zsign").disabled = false;
    });
  });

  $("zsign").addEventListener("click", () => {
    const file = pczt;
    if (!file) return;
    const bar = $<HTMLProgressElement>("zsignbar");
    const line = $("zsignstatus");
    clearSigned();
    void busy("signing a PCZT", async () => {
      const n = network;
      line.textContent = `Sending ${file.name} to the board (${networkLabel(n)}, account ${account})…`;
      bar.hidden = false;
      bar.value = 0;
      const r = await zSign(requireCall(), file.bytes, account, n, (p) => {
        if (p.phase === "upload") {
          bar.value = p.sent / p.total;
        } else {
          bar.hidden = true;
          line.textContent = "Review the transaction on the board. ACCEPT on its last page signs; anything else signs nothing.";
        }
      });
      if (r.entries.length === 0) throw new Error("the board returned no signatures");
      const zsig = encodeZsig(r.sha256, r.entries);
      const where = await save(zsigName(file.name), zsig);
      $("zsighash").textContent = toHex(r.sighash);
      $("zsigcount").textContent = String(r.entries.length);
      $("zsigpath").textContent = where;
      $("zsigned").hidden = false;
      line.textContent = "Signed. Give the .zsig to the wallet that made the PCZT to finish and broadcast.";
      deps.log(`zcash: signed ${file.name} on the board (${r.entries.length} signatures), saved ${where}`);
    }).catch((e) => {
      bar.hidden = true;
      line.textContent = zErrorText(e);
    });
  });

  renderNetwork();

  return {
    reset() {
      clearAccount();
      clearSigned();
      hideWrongNetwork();
    },
    followBoard(n) {
      if (n === network) return false;
      deps.log(`zcash: the board is set to ${networkLabel(n)}; the app follows it`);
      setNetwork(n);
      return true;
    },
    onAccount(f) { accountListeners.push(f); },
    opened() { autoRead(); },
    get network() { return network; },
    get account() { return account; },
    get boardAccount() { return current; },
    get working() { return working; },
    onChange(f) { listeners.push(f); },
    readAccount,
    busy,
  };
}
