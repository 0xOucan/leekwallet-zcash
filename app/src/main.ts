/**
 * LeekWallet Zcash Companion — shell.
 *
 * Talks to a Transport, nothing else: USB or Bluetooth through the Tauri
 * backend, or the in-process mock in a window that has neither. The pairing,
 * unlock, passphrase and status live here; the Zcash screens are
 * src/zcash-view.ts (the board's account and PCZT signer) and
 * src/zcash-wallet.ts (the wallet over zecz). Zcash only: the EVM side of
 * LeekWallet's Companion was removed (docs/JOURNAL.md); git history keeps it.
 */

import { encodeCbor, decodeCbor, type CborValue } from "../packages/core/src/cbor.ts";
import { encodeFrame, FrameDecoder, FrameType } from "../packages/core/src/framing.ts";
import { MockDevice } from "../packages/core/src/mock-device.ts";
import { DeviceError, ErrorCode, type Transport } from "../packages/core/src/transport.ts";
import {
  derivationsInvalidated, parseStatus, UNKNOWN_STATUS, type DeviceStatus,
} from "../packages/core/src/device-state.ts";
import {
  availableTransports, invoker, listPorts, TauriSerialTransport, type HardwareKind,
} from "./tauri-transport.ts";
import { initZcash, networkLabel, type ZcashView } from "./zcash-view.ts";
import { initZcashWallet, type Key, type ZcashWallet } from "./zcash-wallet.ts";
import { initWalletList, type WalletList } from "./wallet-list.ts";
import { zErrorText, zGetFeatures, type ZFeatures } from "../packages/core/src/zcash.ts";
import { BleTransport, scanBle, BLE_NOT_FOUND } from "./ble-transport.ts";
import qrcodegen from "qrcode-generator";
import { initFlasher, tauriFlashBridge } from "./flasher.ts";
import { boardLabel, forgetBoard, loadBoard, saveBoard, type RememberedBoard } from "./board-memory.ts";
import { networkFromStatus } from "./network-guard.ts";
import { closeLeftoverLink, releasedMessage, RELEASED_EVENT } from "./link-events.ts";
import type { ZNetwork } from "../packages/core/src/zcash.ts";
import {
  deriveSession, generateKeypair, generateNonce, verifyCommitment,
  NONCE_BYTES, COMMIT_BYTES, PROTOCOL_VERSION, Session,
} from "../packages/core/src/session.ts";

const $ = <T extends HTMLElement>(id: string): T => {
  const el = document.getElementById(id);
  if (!el) throw new Error(`missing element #${id}`);
  return el as T;
};

/**
 * A line in the device log, newest first, optionally carrying one link.
 *
 * `link` is built by us and never parsed out of `line`. Scanning text for
 * URLs and turning what it finds into anchors is how a log becomes an attack
 * surface; the URL arrives as its own argument or there is no anchor.
 *
 * Each line is an element rather than appended text because an anchor cannot
 * live in a textContent assignment. The anchor's own text is the full URL, so
 * `logText()` still reconstructs exactly what the old plain-text log produced
 * and a pasted diagnostics report is unchanged.
 */
const log = (line: string, link?: string): void => {
  const el = $("log");
  if (el.firstChild !== null && el.textContent === "Nothing yet.") el.textContent = "";
  const stamp = new Date().toLocaleTimeString();
  const row = document.createElement("div");
  row.className = "log-line";
  row.append(document.createTextNode(`${stamp}  ${line}`));
  if (link !== undefined) {
    row.append(document.createTextNode(" — "));
    /* The anchor's text is the whole URL, so `logText()` reproduces exactly
     * what the old plain-text log produced and a pasted report is unchanged. */
    row.append(explorerAnchor(link, link));
  }
  el.prepend(row);
};

/**
 * The log as plain text, newest first — what the diagnostics report pastes.
 *
 * Not `$("log").textContent`: that concatenates the line elements with no
 * separator, which would run every entry of a bug report into one line.
 */
const logText = (): string =>
  Array.from($("log").querySelectorAll(".log-line"))
    .map((row) => row.textContent ?? "")
    .join("\n");

/**
 * Open a URL in the user's real browser, from the desktop app, the Android
 * app or a browser tab.
 *
 * Three routes, tried in order, because none of them works everywhere:
 *
 * 1. **`opener.openUrl`**, the Tauri plugin. The only route that works in the
 *    desktop and Android webviews: both drop a request for a new window, so
 *    `window.open` there returns null and an anchor does nothing at all. The
 *    plugin hands the URL to the OS instead. Its permission in
 *    `capabilities/default.json` is scoped to https.
 * 2. **`window.open`**, which is the right answer in a browser tab, where
 *    there is no Tauri object to find.
 * 3. **The clipboard**, through the same three-route `copyText` the
 *    diagnostics button uses, when a window will do neither.
 *
 * A link that silently does nothing is the failure being avoided here: the
 * user clicks, nothing happens, and there is no way to tell that from an
 * explorer that is merely slow.
 */
async function openExternal(url: string): Promise<void> {
  const opener = (window as unknown as {
    __TAURI__?: { opener?: { openUrl?: (u: string) => Promise<void> } };
  }).__TAURI__?.opener?.openUrl;
  if (opener) {
    try {
      await opener(url);
      return;
    } catch { /* fall through: a refused plugin call is not a reason to stop */ }
  }

  try {
    if (window.open(url, "_blank", "noreferrer") !== null) return;
  } catch { /* a webview that throws is a webview that will not open it */ }

  try {
    const route = await copyText(url);
    announce(
      route === "manual"
        ? "This window cannot open links or reach the clipboard. The explorer URL is in the log — copy it by hand."
        : "This window would not open the link, so the explorer URL was copied to the clipboard instead.",
    );
  } catch {
    announce("This window would not open the link and the copy failed. The explorer URL is in the log.");
  }
}

/** An anchor that opens through openExternal(), never a dead link. */
function explorerAnchor(url: string, label: string): HTMLAnchorElement {
  const a = document.createElement("a");
  a.href = url;
  a.target = "_blank";
  a.rel = "noreferrer";
  a.textContent = label;
  a.addEventListener("click", (ev) => {
    ev.preventDefault();
    void openExternal(url);
  });
  return a;
}

/**
 * Say something out loud, once, to whoever is listening with a screen reader.
 *
 * The region is `role="alert"`, so this interrupts — which is the point, and
 * why the only callers are the moments the hardware is waiting for a person.
 * Everything else belongs in the log, where it can be read at leisure.
 *
 * The cleared-then-set pair is not superstition: assistive technology
 * announces a live region when its contents *change*, and two identical
 * messages in a row — "check every page on the device" for a second
 * transaction, say — are not a change. Clearing first makes the second one
 * arrive, and a repeated instruction that never arrives is precisely the
 * failure mode that matters here.
 */
function announce(text: string): void {
  const region = $("announce");
  region.textContent = "";
  // A microtask is too early: the change has to survive a paint to be noticed.
  setTimeout(() => { region.textContent = text; }, 50);
}

/**
 * The device is waiting for the user. Log it and announce it, same words.
 *
 * Confirming on the device is the security step this whole protocol is built
 * around — comparing a passkey, reading every page of a transaction — and it
 * only works if the user knows they have been asked. A sighted user reads the
 * log; a screen-reader user is given no reason to be in it at that moment, so
 * without this the instruction reaches nobody and the device becomes a button
 * that gets pressed to make the wallet work. That is not an inconvenience, it
 * is the attack the confirmation exists to stop.
 */
const deviceAttention = (line: string): void => { log(line); announce(line); };

/* Errors nobody caught, and resources the CSP refused.
 *
 * These listeners are the only way an exception on a phone -- where there is
 * no console to open -- reaches a human.
 *
 * The CSP listener earns its place separately: this app's policy is a security
 * boundary worth keeping tight, which means a legitimate request can be refused
 * by it, and a refusal is otherwise silent. Naming the blocked URI and the
 * directive turns "nothing happened" into a one-line answer.
 */
window.addEventListener("error", (e) => {
  log(`uncaught error: ${e.message} (${e.filename}:${e.lineno})`);
});
window.addEventListener("unhandledrejection", (e) => {
  const r = e.reason as { message?: string } | undefined;
  log(`unhandled rejection: ${r?.message ?? String(e.reason)}`);
});
document.addEventListener("securitypolicyviolation", (e) => {
  log(`CSP blocked ${e.blockedURI || "(inline)"} — violates ${e.violatedDirective}`);
});

/* console.error, mirrored into the log panel: the console has no reader on a
 * phone. Only the first line and only 200 characters, because this panel is
 * meant to be pasted into bug reports. The console still gets everything,
 * untouched, for desktop devtools.
 */
const consoleError = console.error.bind(console);
console.error = (...args: unknown[]): void => {
  consoleError(...args);
  const first = args
    .map((a) => (a instanceof Error ? a.message : typeof a === "string" ? a : ""))
    .find((t) => t !== "");
  if (first !== undefined) log(`console error: ${(first.split("\n")[0] ?? "").slice(0, 200)}`);
};

/* ------------------------------------------------------------------ client */

/** Minimal request/response client over a Transport. */
class Client {
  private readonly transport: Transport;
  private readonly decoder = new FrameDecoder();
  private pending: ((v: { ok?: Record<string, CborValue>; err?: DeviceError }) => void) | null = null;
  /** Established after the handshake; null while everything is plaintext. */
  private session: Session | null = null;
  /** Serialises requests; see call(). */
  private queue: Promise<void> = Promise.resolve();
  /** True during waitForApproval, when a plaintext 0x0400 means "not yet". */
  private awaitingApproval = false;

  constructor(transport: Transport) {
    this.transport = transport;
    this.transport.onFrame((frame) => {
      for (const f of this.decoder.push(frame)) {
        let payload = f.payload;

        /* Encrypted replies are unsealed before parsing. A failed tag throws
         * and is surfaced rather than retried: a forged frame means the
         * channel is no longer trustworthy. */
        const encrypted =
          f.type === FrameType.EncryptedResponse || f.type === FrameType.EncryptedError;

        /* An encrypted frame with no session to open it is ciphertext, and
         * ciphertext parsed as CBOR is noise - "unsupported major type 7",
         * "trailing bytes", whatever the random bytes happen to spell. Say
         * what actually happened instead of reporting the shape of the
         * garbage. */
        if (encrypted && !this.session) {
          const resolve = this.pending;
          this.pending = null;
          resolve?.({ err: new DeviceError(
            ErrorCode.SessionRequired,
            "the device replied encrypted but this side has no session; reconnect",
          ) });
          continue;
        }

        if (encrypted && this.session) {
          try {
            payload = this.session.decrypt(payload);
          } catch {
            this.killSession("a reply failed authentication");
            const resolve = this.pending;
            this.pending = null;
            resolve?.({ err: new DeviceError(0x0400, "authentication failed") });
            continue;
          }
        }

        const body = decodeCbor(payload) as Record<string, CborValue>;
        const resolve = this.pending;
        this.pending = null;
        if (!resolve) continue;
        if (f.type === FrameType.Error || f.type === FrameType.EncryptedError) {
          const code = Number(body["code"]);
          /* 0x0400 in PLAINTEXT after a session existed means the device threw
           * the session away - session_decrypt() resets on a failed tag, and a
           * failed tag is indistinguishable from an attack, so failing closed
           * there is right. But it leaves this side holding keys the device has
           * forgotten, and every later request gets the same "decrypt failed"
           * against a session that is already gone. Retrying into that is what
           * made an unlock fail twice and look like the PIN was wrong.
           *
           * One corrupted frame is rare on a cable and entirely ordinary on a
           * radio, which is why this only ever showed up over BLE. */
          /* Not while waiting for the button. A device that has not been
           * confirmed yet answers exactly this, in plaintext, every time it is
           * polled - it means "not yet", not "your session is gone". Killing
           * the session here deleted the one that was about to become valid,
           * and the poll then succeeded in plaintext and reported an encrypted
           * channel that did not exist. */
          if (code === ErrorCode.SessionRequired && f.type === FrameType.Error &&
              !this.awaitingApproval) {
            this.killSession("the device ended the session");
          }
          resolve({ err: new DeviceError(code, String(body["message"])) });
        } else {
          resolve({ ok: body["result"] as Record<string, CborValue> });
        }
      }
    });
  }

  /**
   * Send one request and await its reply, one at a time.
   *
   * The link has no request IDs, so a reply belongs to whichever request went
   * out last. With a background poll running, a user action could overlap it
   * and each would resolve the other's promise — and because the nonce counter
   * advances on a completed exchange, the two ends then drift apart and every
   * later frame fails. That is what "clicked Unlock and it hung" was.
   *
   * Serialising here is the fix rather than removing the poll: the poll exists
   * to notice changes made on the device, which is exactly when a user is also
   * touching the app.
   */
  async call(
    method: string,
    params: Record<string, CborValue> = {},
    timeoutMs = 5000,
  ): Promise<Record<string, CborValue>> {
    const mine = this.queue.then(() => this.callNow(method, params, timeoutMs));
    // Keep the chain alive even when a call rejects, or one failure wedges
    // every request that follows.
    this.queue = mine.then(() => undefined, () => undefined);
    return mine;
  }

  private async callNow(
    method: string,
    params: Record<string, CborValue> = {},
    timeoutMs = 5000,
  ): Promise<Record<string, CborValue>> {
    /* Waiting longer than the device does is the only safe direction. If the
     * transport gives up first, the device still processes the request and
     * replies to nobody: its counters move, the host's do not, and the session
     * is unrecoverable. That is not a timeout, it is a broken connection with
     * a misleading message. */
    const t = this.transport as { timeoutMs?: number };
    if ("timeoutMs" in t) t.timeoutMs = timeoutMs;

    const reply = new Promise<{ ok?: Record<string, CborValue>; err?: DeviceError }>((r) => {
      this.pending = r;
    });

    /* Flat, per PROTOCOL.md section 4: fields sit beside `method` rather than
     * inside a `params` object. The firmware looks for them at the top level,
     * so a nested request would have had its arguments silently ignored. */
    const body = encodeCbor({ method, ...params });
    const [type, payload] = this.session?.isActive
      ? [FrameType.EncryptedRequest, this.session.encrypt(body)]
      : [FrameType.Request, body];

    try {
      await this.transport.send(encodeFrame(type, payload));
    } catch (e) {
      /* The device may still be processing. Its counters will have moved and
       * ours have not, so the session cannot be reused - fail loudly rather
       * than leaving the next request to die with "decrypt failed". */
      this.session = null;
      this.pending = null;
      throw new Error(
        `${(e as Error).message}. The session is no longer usable; disconnect and reconnect.`,
      );
    }
    const { ok, err } = await reply;
    if (err) throw err;
    return ok ?? {};
  }

  /**
   * Run the commit-then-reveal handshake and return the passkey to compare.
   *
   * Two round trips, and the order is the security property rather than a
   * formality. The device commits to its nonce in `helloAck`; only then does
   * this side reveal its own in `helloReveal`; only then does the device
   * reveal the nonce it committed to. Neither end could have chosen its
   * contribution after seeing the other's, so a relay between them cannot
   * search for a value that makes both screens agree — it is down to one
   * online guess at 1 in 10^6, which is a mismatch the user sees.
   *
   * Every failure below aborts rather than degrades. A handshake that "worked
   * except for the commitment" is exactly the v1 handshake that a relay ground
   * through in 91 seconds.
   */
  async handshake(): Promise<string> {
    const { privateKey, publicKey } = generateKeypair();
    const ack = await this.call("hello", {
      version: PROTOCOL_VERSION,
      hostPubkey: publicKey,
    });

    /* Version before anything else: an old device answers v1 here, and saying
     * so is far more use than the "decrypt failed" three frames later that a
     * missing check used to produce. */
    const theirVersion = ack["version"];
    if (theirVersion !== PROTOCOL_VERSION) {
      throw new Error(
        `this app speaks protocol v${PROTOCOL_VERSION}; the device answered ` +
        `v${typeof theirVersion === "number" ? theirVersion : "none"} — update the older one`,
      );
    }

    const devicePubkey = ack["devicePubkey"];
    if (!(devicePubkey instanceof Uint8Array) || devicePubkey.length !== 32) {
      throw new Error("device did not return a public key");
    }
    const deviceCommit = ack["deviceCommit"];
    if (!(deviceCommit instanceof Uint8Array) || deviceCommit.length !== COMMIT_BYTES) {
      throw new Error("device did not commit to a nonce");
    }

    const hostNonce = generateNonce();
    const revealed = await this.call("helloReveal", { hostNonce });

    const deviceNonce = revealed["deviceNonce"];
    if (!(deviceNonce instanceof Uint8Array) || deviceNonce.length !== NONCE_BYTES) {
      throw new Error("device did not reveal its nonce");
    }

    /* The check that makes the commitment worth having. Failing it means the
     * nonce was chosen after the device saw ours, which is the whole attack —
     * so this is a refusal, never a warning. */
    if (!verifyCommitment(deviceCommit, devicePubkey, publicKey, deviceNonce)) {
      throw new Error(
        "the device's nonce does not match what it committed to — refusing to " +
        "pair; something is relaying this connection",
      );
    }

    this.session = new Session(
      deriveSession(privateKey, devicePubkey, {
        hostPublic: publicKey,
        devicePublic: devicePubkey,
        hostNonce,
        deviceNonce,
      }),
      "host",
    );
    return this.session.passkey;
  }

  /**
   * Wait for the user to approve on the device.
   *
   * There is no "confirmed" message to wait for: the device simply starts
   * accepting encrypted traffic once the button is pressed. So the host tries
   * an encrypted call until one succeeds, which is both the check and the
   * first real use of the channel.
   */
  async waitForApproval(timeoutMs = 60000): Promise<void> {
    if (!this.session) throw new Error("no handshake");
    this.session.confirm();
    this.awaitingApproval = true;
    try {

      const deadline = Date.now() + timeoutMs;
      for (;;) {
        /* The first encrypted call to succeed IS the confirmation - there is
         * no "confirmed" message to wait for. So it must be an encrypted one:
         * getStatus answers in plaintext too, and a plaintext success here
         * would report a channel that was never established. */
        if (!this.session) {
          throw new Error("the session was lost while waiting for confirmation");
        }
        try {
          await this.call("getStatus");
          return;
        } catch {
          if (Date.now() > deadline) {
            throw new Error("timed out waiting for confirmation on the device");
          }
          await new Promise((r) => setTimeout(r, 750));
        }
      }
    } finally {
      this.awaitingApproval = false;
    }
  }

  /**
   * Forget the session. The next call goes out in plaintext and is refused,
   * which is the honest outcome: there is no channel until a new handshake,
   * and quietly re-establishing one would skip the passkey comparison that
   * makes the channel worth anything.
   */
  private killSession(why: string): void {
    if (!this.session) return;
    this.session = null;
    log(`session ended — ${why}. Reconnect to compare a new passkey.`);
    setConnection("connecting", "Session ended — reconnect");
  }

  get encrypted(): boolean {
    return this.session?.isActive ?? false;
  }
}

/* ------------------------------------------------------------------- state */

let transport: Transport | null = null;
let client: Client | null = null;

/**
 * What the backend says it can do. Empty in a browser tab and on Android,
 * where the mock is the only honest option.
 */
let available: HardwareKind[] = [];

type LinkKind = HardwareKind | "mock";

/** Whatever the Link selector currently reads. */
function selectedKind(): LinkKind {
  const value = ($("transport") as HTMLSelectElement).value;
  return value === "usb" || value === "ble" ? value : "mock";
}

/* Last known device state, and a poll to notice changes the app did not cause
 * - an auto-lock on the device's own timer, or a wallet switched by hand. */
let lastStatus: DeviceStatus = UNKNOWN_STATUS;

/* What the board says it is (getFeatures: firmware, model, chain), read once
 * per connection for the diagnostics report. Null until it answered. */
let deviceFeatures: Record<string, CborValue> | null = null;

let pollTimer: ReturnType<typeof setInterval> | null = null;

/* The board's Settings -> Network, when its getStatus reports it (newer
 * firmware); null on firmware without the field, where a request for the
 * other network is refused with 0x0528 instead (src/network-guard.ts). */
let boardNetwork: ZNetwork | null = null;

/** The app follows the board's network, when the board says which. */
function followBoardNetwork(): void {
  if (boardNetwork !== null && zcashView?.followBoard(boardNetwork)) {
    log(`the board is set to ${networkLabel(boardNetwork)}: the app switched to it`);
  }
}

async function readStatus(): Promise<DeviceStatus> {
  const s = await (client as Client).call("getStatus");
  boardNetwork = networkFromStatus(s["network"]);
  return parseStatus(s);
}

/**
 * Forget everything read from the board under the previous state.
 *
 * Called whenever the device state moves in a way that changes which wallet
 * is open (lock, another wallet, a passphrase, another account): the Zcash
 * account and wallet on screen would otherwise belong to a seed the device
 * is no longer using, and nothing about them would look wrong.
 */
function invalidateDerived(reason: string): void {
  /* setShellVisible(false) hides the wallet and resets the Zcash view. */
  setShellVisible(false);
  log(`wallet view cleared: ${reason}`);
}

let polling = false;

/** Why the previous state no longer applies. Ordered most specific first. */
function invalidationReason(before: DeviceStatus, after: DeviceStatus): string {
  if (!after.unlocked) return "device locked";
  if (!before.unlocked) return "device unlocked";
  if (before.activeWallet !== after.activeWallet) return "wallet changed on device";
  if (before.passphrase !== after.passphrase) {
    return after.passphrase ? "passphrase applied on device" : "passphrase cleared on device";
  }
  if (before.account !== after.account) return `account changed on device to ${after.account}`;
  return "device state changed";
}

/** The one-line wallet state in the header. */
function walletLabel(s: DeviceStatus): string {
  if (!s.unlocked) return "locked";
  /* Not "wallet 0/5". In temporary mode there is no stored wallet to number,
   * and a zero where a wallet number belongs reads as a fault rather than as
   * the feature it is. */
  const wallet = s.temporary
    ? "temp seed (nothing stored)"
    : `wallet ${s.activeWallet}/${s.walletCount}`;
  return wallet + (s.passphrase ? " + passphrase" : "");
}

async function poll(): Promise<void> {
  if (!client) return;

  /* setInterval does not wait: without this guard a slow reply lets the next
   * tick re-enter while the previous run is still working. */
  if (polling) return;
  polling = true;

  try {
    const now = await readStatus();
    const changed = derivationsInvalidated(lastStatus, now);
    const reason = changed ? invalidationReason(lastStatus, now) : "";
    lastStatus = now;

    $("wallet").textContent = walletLabel(now);
    $("passpanel").hidden = !now.unlocked;

    if (changed) {
      invalidateDerived(reason);
      if (now.unlocked) setShellVisible(true);
    }
    /* Switched on the board while connected: the app follows. */
    if (now.unlocked) followBoardNetwork();
  } catch {
    /* A poll failing is not itself news; the connection state covers it. */
  } finally {
    polling = false;
  }
}

const setConnection = (state: string, label: string): void => {
  $("dot").dataset["state"] = state;
  $("conn").textContent = label;
  if (state !== "error") $("connproblem").hidden = true;
};

/**
 * Why the last connection attempt failed, on the Device screen itself.
 *
 * The device log is a closed disclosure at the bottom, and on a phone the
 * reason a USB attempt failed (a permission dialog dismissed, a charge-only
 * cable, a board on the Bluetooth link) is exactly what the user needs in
 * front of them. Every line is also logged, for diagnostics.
 */
function connectProblem(lines: string[]): void {
  for (const line of lines) log(line);
  const box = $("connproblem");
  box.replaceChildren(...lines.map((line) => {
    const p = document.createElement("p");
    p.textContent = line;
    return p;
  }));
  box.hidden = lines.length === 0;
}

/**
 * Put the controls back to their disconnected state.
 *
 * Every failure path in connect() must end here. busy(true) disables all four
 * buttons for the duration of an attempt, and an early return that forgets to
 * undo it leaves the user staring at an app whose Unlock does nothing - which
 * is exactly what happened on hardware: a handshake that failed left every
 * control dead and looked like the session had been lost.
 */
const showDisconnected = (): void => {
  ($("connect") as HTMLButtonElement).disabled = false;
  ($("unlock") as HTMLButtonElement).disabled = true;
  ($("disconnect") as HTMLButtonElement).disabled = true;
  ($("transport") as HTMLSelectElement).disabled =
    ($("transport") as HTMLSelectElement).options.length < 2;
};

const busy = (on: boolean): void => {
  for (const id of ["connect", "unlock", "disconnect"]) {
    ($(id) as HTMLButtonElement).disabled = on;
  }
};

/**
 * A wallet opened without the board, from the launcher's list
 * (src/wallet-list.ts): the wallet screens show it until it is closed or
 * a board is connected (its account takes over).
 */
function openOffline(key: Key): void {
  if (!zcashWallet || !zcashView) return;
  zcashView.useNetwork(key.network);
  zcashWallet.openWithoutBoard(key);
  preConnectDest = "offline";
  applyPreConnectVisibility();
  $("zoffline").hidden = false;
  $("seczcash").hidden = false;
}

/** Close a wallet opened without the board; nothing else on screen moves. */
function leaveOffline(): void {
  if (zcashWallet?.offline) zcashWallet.openWithoutBoard(null);
  $("zoffline").hidden = true;
}

/** The launcher's wallet list, when this build has the wallet. */
let walletList: WalletList | null = null;

/** The Zcash section, once initialised (src/zcash-view.ts). */
let zcashView: ZcashView | null = null;
/** The wallet, when this build has it (src/zcash-wallet.ts). */
let zcashWallet: ZcashWallet | null = null;

/**
 * Show or hide the home screen: the Zcash wallet.
 *
 * Called from exactly the places where the board's state says there is (or
 * is no longer) an unlocked wallet to show: unlock, a host-side passphrase,
 * a poll that saw the state move, and disconnect.
 *
 * This is a safety behaviour, named here: a
 * locked or disconnected device must never leave an address or a send form
 * on screen. Hiding #seczcash hides everything the wallet drew, and the
 * Zcash view forgets the account it read.
 */
function setShellVisible(visible: boolean): void {
  $("seczcash").hidden = !visible;
  document.documentElement.dataset["shell"] = visible ? "open" : "closed";
  /* The Zcash wallet reads the board's account as soon as the home screen
   * is up: its seed fingerprint picks which wallet on this device is shown
   * (another one under a passphrase). */
  if (visible) {
    leaveOffline();
    zcashView?.opened();
  }
  // Nothing the board said about its Zcash account outlives the session.
  else zcashView?.reset();
}

/**
 * The launcher (L1).
 *
 * Two destinations, entered and left one at a time — "one thing per screen"
 * per §2e: connect a device, or flash its firmware. The home screen (above)
 * is what follows a connected, unlocked board; this is only about what is on
 * screen before that.
 */
type PreConnectDest = "launcher" | "connect" | "flash" | "wallets" | "offline";
let preConnectDest: PreConnectDest = "launcher";

/** Show exactly the launcher, or exactly the one destination chosen. */
function applyPreConnectVisibility(): void {
  $("launcher").hidden = preConnectDest !== "launcher";
  renderRemembered();
  $("devicepanel").hidden = preConnectDest !== "connect";
  $("flashpanel").hidden = preConnectDest !== "flash";
  $("walletspanel").hidden = preConnectDest !== "wallets";
  if (preConnectDest === "wallets") void walletList?.refresh();
}

/** Flash holds no session, so leaving it is never more than
 * a screen change — no teardown, unlike the connect destination below. */
function goToLauncher(): void {
  preConnectDest = "launcher";
  applyPreConnectVisibility();
}

function enterDest(d: PreConnectDest): void {
  preConnectDest = d;
  applyPreConnectVisibility();
}

/* Guards a concurrent Back click the same way `connecting` guards a second
 * Connect click (see its own comment above, and the two-handshake bug it
 * fixed) — disconnect() awaits transport.close() before it clears `client`,
 * so a second click in that window would still see a session to tear down
 * and start a second, overlapping disconnect(). */
let backingOut = false;

/**
 * The connect destination's Back button.
 *
 * Unlike Flash, this destination can be mid-handshake or
 * fully connected when Back is pressed, and there is no safe way to just
 * hide the panel under either condition: a hidden Device panel with a live
 * transport is a session with no Disconnect button. So Back reuses
 * disconnect() itself — the same teardown the visible Disconnect button
 * calls — rather than inventing a second way to end a session that could
 * disagree with the first about what "ended" means.
 *
 * `connecting` is checked first because disconnect() is not safe to run
 * concurrently with connectOnce(): both would end up touching the same
 * `transport`/`client` variables from two different in-flight async
 * functions, which is exactly the shape of the race documented on
 * `connecting` above.
 */
function connectDestBack(): void {
  if (connecting || backingOut) return;
  if (client) {
    backingOut = true;
    void disconnect().finally(() => { backingOut = false; });
  } else {
    goToLauncher();
  }
}
/** The launcher's "Reconnect" tile and the Device panel's remembered line. */
function renderRemembered(): void {
  const b = loadBoard();
  const usable = b !== null && (available as string[]).includes(b.link);
  $("gotoreconnect").hidden = !usable;
  $("rememberline").hidden = !usable;
  if (b && usable) {
    $("reconnectdesc").textContent = boardLabel(b);
    $("remembername").textContent = boardLabel(b);
  }
}

/** One tap: the remembered board, over its link, then the PIN, then the wallet. */
function reconnect(): void {
  const b = loadBoard();
  if (!b) { enterDest("connect"); return; }
  enterDest("connect");
  const select = $("transport") as HTMLSelectElement;
  if (Array.from(select.options).some((o) => o.value === b.link)) select.value = b.link;
  void connect(b);
}

/* ----------------------------------------------------------------- actions */

/**
 * Find a device on the selected link.
 *
 * Returns null having already explained itself, because the two failures need
 * different explanations: no serial port is usually a cable or a permissions
 * problem, while nothing advertising over BLE is usually a device that is
 * simply in USB mode and therefore silent on the radio (PROTOCOL.md 3b).
 * Collapsing both into "no device found" would send someone hunting a fault
 * that does not exist.
 */

/**
 * Whether the USB port we last opened was the board's own USB.
 *
 * False for a UART bridge — CH343, CP210x, FTDI — which is a port the protocol
 * can never answer on, since protocol.c installs its endpoint on
 * USB-Serial-JTAG alone. Remembered because the silence that follows is
 * diagnosed far from where the port is chosen, and the two causes of that
 * silence need opposite advice.
 */
let usbPortWasNative = true;

async function findDevice(kind: LinkKind, remembered: RememberedBoard | null = null): Promise<Transport | null> {
  /* The remembered Bluetooth board: straight to it by id (ble_connect scans
   * for exactly that peer), so a second board in range is never picked. */
  if (kind === "ble" && remembered?.link === "ble" && remembered.bleId) {
    log(`reconnecting to ${remembered.bleName ?? remembered.bleId}`);
    return new BleTransport({ id: remembered.bleId, name: remembered.bleName ?? null });
  }
  if (kind === "usb") {
    /* Enumeration *rejects* rather than returning empty when the platform has
     * something specific to say - on Android, the cable and USB-host checks
     * that a laptop's dialout-group advice would send someone past. Same shape
     * as the BLE branch below, and for the same reason: swallowing the message
     * would leave the user staring at an empty list. */
    let ports;
    try {
      ports = await listPorts();
    } catch (e) {
      setConnection("error", "No device over USB");
      connectProblem([`USB: ${String((e as Error).message ?? e)}`]);
      return null;
    }
    const port = ports.find((p) => p.likely_device) ?? ports[0];
    if (!port) {
      setConnection("error", "No device over USB");
      connectProblem(["No USB serial port found. Check the cable carries data (not charge-only), and on Linux that you are in the dialout group."]);
      return null;
    }
    /* More than one candidate is unusual and worth saying out loud rather than
     * silently taking the first - the same rule the BLE branch follows. It is
     * not hypothetical on a phone: a USB-C dock or a second dev board is a
     * whole extra Espressif device on the bus, and the user should know a
     * choice was made for them before they approve a signature on it. */
    const candidates = ports.filter((p) => p.likely_device);
    if (candidates.length > 1) {
      log(`${candidates.length} Espressif devices attached; using ${port.name}`);
    } else if (!port.likely_device) {
      /* Worth naming precisely, because the usual cause is a cable in the
       * wrong socket rather than a missing board. An ESP32-S3 dev board has
       * TWO USB-C ports: the native one (Espressif VID 303a) carries
       * USB-Serial-JTAG, and a second one sits behind a UART bridge — CH343,
       * CP210x, FTDI. protocol.c installs the endpoint on USB-Serial-JTAG and
       * nothing else, so on the bridge port the device cannot answer at all:
       * there is no driver listening, and no Link setting changes that.
       * Saying "no Espressif device" alone sent a user hunting through the
       * device's Bluetooth settings for an hour. */
      log(`no Espressif device among ${ports.length} port(s); trying ${port.name} anyway`);
      log(`${port.name} is a USB-UART bridge, not the board's native USB.`);
      log(`If this is an ESP32-S3, move the cable to its other USB-C socket —`);
      log(`the one that enumerates as "Espressif USB JTAG/serial debug unit".`);
    }
    usbPortWasNative = port.likely_device;
    log(`found ${port.name} — ${port.description}`);
    /* A port existing does not mean the device is listening on it. With Link
     * set to Bluetooth the firmware drains this port and parses nothing
     * (PROTOCOL.md 3b), so the cable enumerates exactly as it always does and
     * every request goes unanswered. There is no way to ask the device which
     * link it is on over the link it has switched off, so the silence is the
     * signal - see the handshake timeout below. */
    return new TauriSerialTransport(port.name);
  }

  if (kind === "ble") {
    log("scanning for Bluetooth devices…");
    let found;
    try {
      found = await scanBle();
    } catch (e) {
      setConnection("error", "Bluetooth unavailable");
      connectProblem([`Bluetooth: ${String((e as Error).message ?? e)}`]);
      return null;
    }
    const device = found[0];
    if (!device) {
      setConnection("error", "No device over Bluetooth");
      connectProblem([BLE_NOT_FOUND]);
      return null;
    }
    // More than one is unusual and worth saying out loud rather than silently
    // taking the first: the user should know a choice was made for them.
    if (found.length > 1) {
      log(`${found.length} devices advertising; using ${device.name ?? device.id}`);
    }
    log(`found ${device.name ?? "(unnamed)"} — ${device.id}`);
    return new BleTransport(device);
  }

  /* Latency is deliberate: a mock that answers instantly hides every place
   * the UI forgot to show that it is waiting. `pinEntryMs` is the same idea
   * applied to unlocking - the device only prompts, and the seconds the user
   * spends on the keypad are seconds this app has to keep polling and keep
   * saying so. */
  return new MockDevice({ latencyMs: 250, walletCount: 1, pinEntryMs: 2000 });
}

/**
 * Whether a connection attempt is already in flight.
 *
 * A guard rather than a disabled button, because the button is only disabled
 * once `busy(true)` runs — and that is after the scan has been awaited, which
 * is precisely the several-second window a user is most likely to press it
 * again in. Two attempts overlapping is not two chances at the same thing: each
 * runs its own X25519 handshake, the device resets its session on every
 * handshake and therefore keeps the LAST key, and the app goes on encrypting
 * with the first. Every request after that fails to decrypt, and no amount of
 * retrying helps because both ends are behaving correctly with different keys.
 *
 * That state cost a user a full restart of the companion AND the board to
 * clear. The log said it plainly in hindsight: two "scanning" lines a second
 * apart, then two handshakes offering different passkeys.
 *
 * Set synchronously before the first await, so two clicks in the same tick
 * cannot both pass it.
 */
let connecting = false;

async function connect(remembered: RememberedBoard | null = null): Promise<void> {
  if (connecting) {
    log("already connecting — ignoring that");
    return;
  }
  connecting = true;
  let paired = false;
  try {
    paired = await connectOnce(remembered);
  } finally {
    connecting = false;
  }
  /* The guided flow: paired, so ask the board for its PIN right away (an
   * unlocked board answers at once). Once unlocked, the wallet opens and
   * syncs by itself (zcash-wallet.ts). */
  if (paired) await unlock();
}

async function connectOnce(remembered: RememberedBoard | null = null): Promise<boolean> {
  /* Real hardware over whichever link the user picked, the mock when there is
   * none. All three are interchangeable by construction — if they were not,
   * everything built against the mock would need revisiting the first time a
   * device was plugged in, and everything proven over USB would prove nothing
   * about BLE. */
  const kind = selectedKind();
  const found = await findDevice(kind, remembered);
  if (!found) return false;
  transport = found;
  client = new Client(transport);
  setMode(transport);

  setConnection("connecting", "Connecting…");
  busy(true);
  try {
    await transport.open();
  } catch (e) {
    setConnection("error", "Connection failed");
    connectProblem([`Opening ${found.label} failed: ${String((e as Error).message ?? e)}`]);
    transport = null;
    client = null;
    setMode(null);
    showDisconnected();
    return false;
  }

  if (kind !== "mock") {
    /* Nothing stale may survive into a new attempt. A passkey left on screen
     * from a previous session is worse than none: the whole point is that the
     * user compares it against the device, and a number the device is not
     * showing invites them to conclude the comparison failed for some other
     * reason - or, worse, to stop checking. */
    $("passkey").textContent = "";
    $("pairing").hidden = true;

    /* Real firmware: derive the passkey from the exchange and wait for the
     * user to compare it against the OLED. */
    let passkey: string;
    try {
      passkey = await client.handshake();
    } catch (e) {
      /* On USB this is nearly always a device whose Link is set to Bluetooth:
       * the port opened, the bytes went out, and nothing was listening. */
      setConnection("error", "Device did not answer");
      /* The transport's own words first: on Android a USB read error, a
       * detached device or a refused claim reads very differently from the
       * plain timeout the advice below is about, and swallowing it left a
       * phone user with only the advice. */
      const why = `Handshake over ${found.label}: ${String((e as Error).message ?? e)}`;
      if (kind === "usb" && !usbPortWasNative) {
        /* The likelier of the two causes, and the one the user cannot guess:
         * a bridge port has no protocol endpoint behind it at all. Say this
         * INSTEAD of the Bluetooth advice, not after it — two explanations for
         * one silence is how an hour goes missing. */
        connectProblem([
          why,
          "The port opened but the device never answered. That port is a USB-UART bridge: the protocol runs on the board's native USB only, so nothing is listening there whatever the device's Link setting says. Move the cable to the other USB-C socket on the board and connect again.",
        ]);
      } else if (kind === "usb") {
        connectProblem([
          why,
          "The port opened but the device never answered. The board serves one link at a time: if its Settings → Link is Bluetooth, USB is silent by design. Set Link to USB on the board (or switch this app to Bluetooth) and connect again.",
        ]);
      } else {
        connectProblem([why]);
      }
      await transport.close().catch(() => {});
      transport = null;
      client = null;
      setMode(null);
      showDisconnected();
      return false;
    }
    $("passkey").textContent = `${passkey.slice(0, 3)} ${passkey.slice(3)}`;
    $("pairing").hidden = false;
    log(`handshake done — compare ${passkey} with the device screen`);
    log("press ALLOW on the device to continue");
    /* Announced rather than routed through deviceAttention, because the spoken
     * form has to differ from the logged one: the passkey lives in a definition
     * list a screen-reader user has no reason to be in, and six digits read as
     * one number are not a string anybody can compare against a device screen.
     * So it is spoken separately, and separated. */
    announce(
      `Compare the passkey ${passkey.split("").join(" ")} with the device screen, ` +
      `then press ALLOW on the device to continue.`,
    );
    setConnection("connecting", "Confirm on device…");

    try {
      await client.waitForApproval();
    } catch (e) {
      /* Times out after a minute, or the session died while waiting. Either
       * way this attempt is over and the controls must come back - the user
       * has to be able to press Connect again. */
      setConnection("error", "Not confirmed on the device");
      announce("Not confirmed on the device. The connection attempt has ended.");
      log(String((e as Error).message ?? e));
      await transport.close().catch(() => {});
      transport = null;
      client = null;
      setMode(null);
      showDisconnected();
      return false;
    }
    log("approved on device; channel encrypted");
  } else {
    /* The mock has no key agreement, so this is not handshake() — but it is
     * still both legs in the right order, because the mock models the state
     * machine and refuses a reveal that no commitment is waiting on. Anything
     * less here would be the demo path quietly proving the protocol works with
     * one round trip. */
    await client.call("hello", { version: PROTOCOL_VERSION });
    const hello = await client.call("helloReveal");
    const passkey = hello["passkey"];
    $("passkey").textContent =
      typeof passkey === "string" ? passkey : "(mock: nothing to compare)";
    $("pairing").hidden = false;
    log(`session established with ${transport.label}`);
  }

  /* What the board says it is, once, for the diagnostics report. Plaintext
   * is fine here: getFeatures describes the device, not the wallet. */
  deviceFeatures = await client.call("getFeatures").catch(() => null);

  setConnection("connected", transport.label);
  $("devicehint").textContent = kind !== "mock"
    ? `Connected over ${transport.label}. The device confirms everything it signs on its own screen.`
    : "Connected to the mock. It models pairing, the PIN and the passphrase, and holds no keys: the Zcash screens need a board (or zdev, the simulated one).";
  busy(false);
  ($("connect") as HTMLButtonElement).disabled = true;
  // Switching links mid-session would tear down the device's session anyway;
  // shutting the selector says so before it happens.
  ($("transport") as HTMLSelectElement).disabled = true;
  ($("unlock") as HTMLButtonElement).disabled = false;
  ($("disconnect") as HTMLButtonElement).disabled = false;

  /* Remembered for a one-tap reconnect: the link and, over Bluetooth, the
   * peer's id and name. Nothing of the session (board-memory.ts). */
  if (kind === "usb") saveBoard({ link: "usb", at: Date.now() });
  else if (transport instanceof BleTransport) {
    saveBoard({ link: "ble", bleId: transport.device.id, bleName: transport.device.name, at: Date.now() });
  }
  return true;
}
async function unlock(): Promise<void> {
  if (!client) return;
  busy(true);
  deviceAttention("enter your PIN on the device…");
  try {
    const reply = await client.call("unlock");

    /* The device only *prompts*; the PIN is typed there and never travels, so
     * the reply is `{prompted:1, unlocked:0}` and arrives long before the user
     * has touched the keypad. The status poll is the answer. `{unlocked:1}`
     * comes back only when the device was already unlocked. */
    if (reply["unlocked"] !== 1) {
      log("waiting for the PIN on the device…");
      const deadline = Date.now() + 120000;
      for (;;) {
        await new Promise((r) => setTimeout(r, 750));
        const s = await readStatus();
        if (s.unlocked) break;
        if (Date.now() > deadline) throw new Error("timed out waiting for the PIN");
      }
    }
    log("device unlocked");
    lastStatus = await readStatus();
    /* Before the home screen reads the account: on the board's network,
     * when it says which, so nothing is refused for the other one. */
    if (lastStatus.unlocked) followBoardNetwork();
    setShellVisible(lastStatus.unlocked);
    $("passpanel").hidden = !lastStatus.unlocked;
    if (!lastStatus.unlocked) {
      log("device is locked — press Unlock, then enter your PIN on the device");
    }
    $("wallet").textContent = walletLabel(lastStatus);

    /* Two seconds is frequent enough that a lock is noticed before the user
     * acts on a stale address, and rare enough not to keep a BLE link busy. */
    if (!pollTimer) pollTimer = setInterval(() => void poll(), 2000);
  } catch (e) {
    // Name the thing that failed. "undefined" was the previous message when
    // the device answered with an error carrying no text.
    const msg = e instanceof DeviceError ? e.message
      : e instanceof Error && e.message ? e.message
      : String(e);
    log(`unlock failed: ${msg}`);
  } finally {
    busy(false);
    ($("connect") as HTMLButtonElement).disabled = true;
  }
}

/* -------------------------------------------------------------- passphrase */

/*
 * Host-side passphrase entry (T40) — the weaker of the two paths, and labelled
 * as such everywhere it is reachable.
 *
 * The device already accepts `setPassphrase` and already refuses to apply one
 * without a confirmation on its own screen. What was missing was any way to
 * send one, which meant the choice between the two paths was not being offered
 * — and an unoffered choice is not a security decision anyone made.
 *
 * So it is offered, and the accounting is spelled out at the point of use
 * rather than in a document: this app reads every character before encryption
 * touches it, so a compromised host learns the passphrase, and no amount of
 * ChaCha20 on the wire changes that. On-device entry stays the default and the
 * one described first; this lives behind a closed disclosure with the warning
 * inside it.
 *
 * Nothing here keeps the passphrase: it is read out of the field, handed to
 * the transport, and the field is cleared in a finally. It is never logged,
 * never announced, never persisted, and diagnosticsReport() has no path to it.
 */

/** Matches protocol.c: printable ASCII, 1..63 bytes, no empty (that is not "clear"). */
function passphraseComplaint(value: string): string | null {
  if (value.length === 0) return "Nothing to send. An empty passphrase is the base wallet, not a passphrase.";
  /* Byte length, because the device's bound is a 64-byte buffer. Non-ASCII is
   * refused a line below anyway, so this only ever matters for long input. */
  if (new TextEncoder().encode(value).length > 63) return "Too long: the device accepts up to 63 characters.";
  for (const ch of value) {
    const code = ch.codePointAt(0) ?? 0;
    if (code < 0x20 || code > 0x7e) {
      /* Named, not shown: echoing the offending character back into the page
       * would put a piece of the passphrase on screen and into a screenshot. */
      return "Only printable ASCII, the same set the device's own keyboard can type.";
    }
  }
  return null;
}

function initPassphrase(): void {
  const input = $("passinput") as HTMLInputElement;
  const button = $("passapply") as HTMLButtonElement;
  const hint = $("passhint");

  const apply = async (): Promise<void> => {
    if (!client) { hint.textContent = "No device connected."; return; }

    const complaint = passphraseComplaint(input.value);
    if (complaint) { hint.textContent = complaint; return; }

    button.disabled = true;
    hint.textContent = "Waiting for the device…";
    deviceAttention(
      "check the fingerprint (XFP) the device shows, then confirm the passphrase on the device",
    );
    try {
      /* The value goes straight from the field into the call. No local, no
       * closure holding it after this line, and the field is emptied in the
       * finally below whichever way this ends. */
      const reply = await client.call("setPassphrase", { passphrase: input.value }, 150000);

      /* The device answers with the BIP32 master fingerprint (XFP) of the
       * seed it derived, the number it drew on its screen as "XFP"
       * (protocol.c, setPassphrase), because that is what the user compares.
       * Not a secret. */
      const fp = reply["fingerprint"];
      const shown = typeof fp === "number"
        ? fp.toString(16).toUpperCase().padStart(8, "0")
        : "";
      hint.textContent = shown
        ? `Applied. The device shows XFP ${shown} — if that is not the wallet you expected, the passphrase was mistyped: lock the device to drop it.`
        : "Applied. Check the XFP against the one the device showed.";
      log("passphrase applied from the host and confirmed on the device");

      /* Invalidate by hand rather than waiting for the poll. Replacing one
       * passphrase with another leaves the status flag reading `true` both
       * before and after, so `derivationsInvalidated()` sees no change and the
       * previous seed's wallet would stay on screen. That is exactly the case
       * this rule exists for, and it is the one a boolean cannot see. */
      invalidateDerived("passphrase changed from the host");
      lastStatus = await readStatus();
      $("wallet").textContent = walletLabel(lastStatus);
      if (lastStatus.unlocked) setShellVisible(true);
    } catch (e) {
      const msg = e instanceof DeviceError ? e.message
        : e instanceof Error && e.message ? e.message
        : String(e);
      /* The device drops an unconfirmed passphrase itself, so a refusal here
       * leaves the base wallet rather than a half-applied one. Say so: "it
       * failed" without "and you are still where you were" is what makes
       * someone try again blind. */
      hint.textContent = `Not applied: ${msg}. The device kept the wallet it was already in.`;
      log(`passphrase not applied: ${msg}`);
    } finally {
      input.value = "";
      button.disabled = false;
    }
  };

  button.addEventListener("click", () => void apply());
  /* Enter in a single-field form is what everyone types, and a field that
   * ignores it gets its contents submitted twice by a user hunting the button. */
  input.addEventListener("keydown", (e) => {
    if (e.key === "Enter") { e.preventDefault(); void apply(); }
  });
}

/* ---------------------------------------------------------------------- QR */

/**
 * Renders a QR as an SVG path. No canvas, no raster, scales to any size.
 *
 * Drawn from what the board or the wallet reported, which makes it a
 * convenience and not evidence: a tampered app could draw anyone's address.
 * The board draws the same address on its own screen ("Verify on the
 * board"), and that is the copy worth checking before receiving anything.
 */
function qrSvg(text: string, mode?: "Alphanumeric"): SVGSVGElement {
  /* Error correction M: a receive address on a screen is not a label on a
   * warehouse crate, so the extra redundancy of Q/H buys little, while a
   * smaller matrix stays readable on a phone held at arm's length. Type 0 lets
   * the library pick the smallest version that fits. */
  const qr = qrcodegen(0, "M");
  qr.addData(text, mode);
  qr.make();

  const n = qr.getModuleCount();
  const quiet = 4;               // the spec's mandatory quiet zone
  const size = n + quiet * 2;

  const svg = document.createElementNS("http://www.w3.org/2000/svg", "svg");
  svg.setAttribute("viewBox", `0 0 ${size} ${size}`);
  svg.setAttribute("width", "220");
  svg.setAttribute("height", "220");
  svg.setAttribute("role", "img");
  svg.setAttribute("aria-label", `QR code for ${text}`);

  const bg = document.createElementNS("http://www.w3.org/2000/svg", "rect");
  bg.setAttribute("width", String(size));
  bg.setAttribute("height", String(size));
  /* White, always, whatever the page theme is doing. A scanner needs the
   * contrast in the right direction and a dark-mode QR is a support ticket. */
  bg.setAttribute("fill", "#ffffff");
  svg.appendChild(bg);

  let d = "";
  for (let y = 0; y < n; y++) {
    for (let x = 0; x < n; x++) {
      if (qr.isDark(y, x)) d += `M${x + quiet} ${y + quiet}h1v1h-1z`;
    }
  }
  const path = document.createElementNS("http://www.w3.org/2000/svg", "path");
  path.setAttribute("d", d);
  path.setAttribute("fill", "#000000");
  svg.appendChild(path);
  return svg;
}


async function disconnect(): Promise<void> {
  if (pollTimer) { clearInterval(pollTimer); pollTimer = null; }
  lastStatus = UNKNOWN_STATUS;
  deviceFeatures = null;
  boardNetwork = null;
  await transport?.close().catch((e: unknown) => log(`closing the link: ${String((e as Error).message ?? e)}`));
  transport = null;
  client = null;
  setConnection("disconnected", "Disconnected");
  setMode(null);
  // Back to whatever the build allows: a single-option selector stays shut.
  ($("transport") as HTMLSelectElement).disabled =
    ($("transport") as HTMLSelectElement).options.length < 2;
  $("passpanel").hidden = true;
  $("pairing").hidden = true;
  setShellVisible(false);
  /* The field holds nothing between calls, but a disconnect is exactly when a
   * half-typed one would otherwise be left sitting in the DOM. */
  ($("passinput") as HTMLInputElement).value = "";
  $("passhint").textContent = "";
  $("wallet").textContent = "";
  ($("connect") as HTMLButtonElement).disabled = false;
  ($("unlock") as HTMLButtonElement).disabled = true;
  ($("disconnect") as HTMLButtonElement).disabled = true;
  log("disconnected; session secrets cleared");
  // Symmetric with connectOnce()'s "the app moves itself to the wallet
  // screen": losing the session moves it back to
  // the launcher, whether Disconnect was pressed directly or via the connect
  // destination's Back button (connectDestBack, above).
  goToLauncher();
}

/* ------------------------------------------------------------------- wiring */

const LINK_NAMES: Record<LinkKind, string> = {
  usb: "USB cable",
  ble: "Bluetooth",
  mock: "Mock device",
};

/**
 * Say what the badge actually knows, and no more.
 *
 * Before a connection it can only report what this build could do; after one
 * it names the live transport. Naming it matters: a user comparing an address
 * against the device screen is checking that the thing on the desk produced
 * it, and "hardware" alone does not say which wire that answer came down.
 */
function setMode(active: Transport | null): void {
  const badge = $("mode");
  if (active) {
    const hardware = active.kind !== "mock";
    badge.textContent = hardware
      ? `hardware · ${active.kind.toUpperCase()}`
      : "mock";
    badge.dataset["mode"] = hardware ? "hardware" : "mock";
    badge.title = hardware
      ? `Connected to real hardware over ${LINK_NAMES[active.kind]}`
      : "Simulated device — nothing here is signed by hardware";
    /* A `title` is a mouse hover and nothing else. Whether the signature you
     * are about to trust came from hardware or from a simulation is the last
     * thing that should be reachable only with a pointer. */
    badge.setAttribute("aria-label", badge.title);
    return;
  }

  const hasHardware = available.length > 0;
  badge.textContent = hasHardware ? "no link" : "mock";
  badge.dataset["mode"] = hasHardware ? "idle" : "mock";
  badge.title = hasHardware
    ? "Not connected. The badge names the transport once a device answers."
    : "This build has no device transport; the mock is the only option";
  badge.setAttribute("aria-label", badge.title);
}

/* Say plainly what this window can talk to, before anything is connected. The
 * label used to read "Connect mock device" unconditionally, so a native window
 * talking to real hardware still claimed to be a simulation. */
async function initEnvironment(): Promise<void> {
  available = await availableTransports();

  const select = $("transport") as HTMLSelectElement;
  select.textContent = "";
  /* The mock is offered only when there is no real transport. On a desktop
   * build it would be an option sitting one mis-click away from the hardware
   * one, and a fake device the user believes is real is worse than no device
   * at all. */
  const kinds: LinkKind[] = available.length > 0 ? [...available] : ["mock"];
  for (const kind of kinds) {
    const opt = document.createElement("option");
    opt.value = kind;
    opt.textContent = LINK_NAMES[kind];
    select.appendChild(opt);
  }
  /* USB first: it is the link the device ships selected, and the one a user
   * with a cable in hand is most likely to want. */
  select.value = kinds[0] as string;
  select.disabled = kinds.length < 2;

  ($("connect") as HTMLButtonElement).textContent =
    available.length > 0 ? "Connect device" : "Connect mock device";

  $("devicehint").textContent = available.length > 0
    ? "Pick the link the device is set to, then Connect. The device serves only one at a time — if it is set to USB it will not appear over Bluetooth, and the reverse. Confirm the passkey on the device when asked."
    /* Reaches a browser tab and an Android window alike, so it cannot name a
     * cause it does not know. What matters is the same either way: nothing
     * here is signed by a device. */
    : "No device transport available, so this uses the built-in mock. It speaks the firmware's session protocol (pairing, PIN, passphrase) and holds no keys: nothing is signed, and the Zcash screens need a real board.";

  setMode(null);
  ($("connect") as HTMLButtonElement).disabled = false;
  renderRemembered();
}

/* Held shut until the backend has answered. Clicking through an unpopulated
 * selector would read as "mock" and quietly connect to a simulation on a
 * machine that has a device attached. */
($("connect") as HTMLButtonElement).disabled = true;
/* A link the backend still holds from before this webview started has no
 * session behind it and keeps the board from advertising: closed first
 * (src/link-events.ts, src-tauri/src/link.rs). */
void closeLeftoverLink(invoker()).then((line) => { if (line) log(line); }).finally(() => void initEnvironment());

/* The backend closed the link while the app was in the background (or is
 * closing): the session on screen is over, said in words. */
{
  type Listen = <T>(event: string, cb: (e: { payload: T }) => void) => Promise<unknown>;
  const listen = (window as unknown as { __TAURI__?: { event?: { listen?: Listen } } }).__TAURI__?.event?.listen;
  void listen?.<{ reason: string }>(RELEASED_EVENT, (e) => {
    const line = releasedMessage(e.payload?.reason ?? "");
    if (!client) { log(line); return; }
    void disconnect().finally(() => {
      setConnection("disconnected", "Link closed — reconnect");
      log(line);
      announce(line);
    });
  });
}
initPassphrase();
initFlasher(tauriFlashBridge(log));
applyPreConnectVisibility();
setShellVisible(false);

$("gotoconnect").addEventListener("click", () => enterDest("connect"));
$("gotoreconnect").addEventListener("click", reconnect);
$("forgetboard").addEventListener("click", () => {
  forgetBoard();
  renderRemembered();
  log("forgot the remembered board");
});
$("gotoflash").addEventListener("click", () => enterDest("flash"));
$("connectback").addEventListener("click", connectDestBack);
$("flashback").addEventListener("click", goToLauncher);
$("gotowallets").addEventListener("click", () => enterDest("wallets"));
$("walletsback").addEventListener("click", goToLauncher);
$("zofflineclose").addEventListener("click", () => {
  leaveOffline();
  $("seczcash").hidden = true;
  enterDest("wallets");
});
$("zofflineconnect").addEventListener("click", () => {
  leaveOffline();
  $("seczcash").hidden = true;
  enterDest("connect");
});

/* Zcash (src/zcash-view.ts). Its calls ride the Client like every other
 * request, and only once the session is encrypted: every z* method is in
 * the board's keys tier, which refuses plaintext. */
zcashView = initZcash({
  call: () => (client?.encrypted ? (m, p, t) => (client as Client).call(m, p, t) : null),
  log,
  copy: (text) => copyText(text),
  qr: (text) => qrSvg(text),
  invoke: invoker,
});
/* The wallet (sync, send, history), when this build has it: src-tauri's
 * zwallet.rs over zecz. A build without it keeps the board tools above. */
void initZcashWallet({
  call: () => (client?.encrypted ? (m, p, t) => (client as Client).call(m, p, t) : null),
  log,
  copy: (text) => copyText(text),
  qr: (text) => qrSvg(text),
  invoke: invoker,
  open: (url) => void openExternal(url),
}, zcashView).then((w) => {
  zcashWallet = w;
  if (!w) return;
  walletList = initWalletList({ invoke: invoker()!, open: openOffline, log });
  $("gotowallets").hidden = false;
});

$("connect").addEventListener("click", () => void connect());
$("unlock").addEventListener("click", () => void unlock());
$("disconnect").addEventListener("click", () => void disconnect());

/* ------------------------------------------------------------------- theme */

/* Three states, not two. "System" has to be reachable, or a user who toggles
 * once can never get back to following their OS. Reading the computed --bg and
 * comparing it to a literal would also break the moment the palette moves. */
type Theme = "system" | "light" | "dark";
const THEME_KEY = "leek.theme";
const THEME_ORDER: Theme[] = ["system", "light", "dark"];
const THEME_ICON: Record<Theme, string> = { system: "◐", light: "☀", dark: "☾" };

function applyTheme(theme: Theme): void {
  const root = document.documentElement;
  if (theme === "system") {
    delete root.dataset["theme"];
  } else {
    root.dataset["theme"] = theme;
  }
  const btn = $("theme");
  btn.textContent = THEME_ICON[theme];
  btn.setAttribute("aria-label", `Theme: ${theme}. Click to change.`);
  btn.title = `Theme: ${theme}`;
}

function currentTheme(): Theme {
  const stored = localStorage.getItem(THEME_KEY);
  return stored === "light" || stored === "dark" || stored === "system" ? stored : "system";
}

applyTheme(currentTheme());

/* ------------------------------------------------------------ diagnostics */

/**
 * Everything a bug report needs, as plain text.
 *
 * Every bug report on this project so far has been the log, hand-copied off a
 * screen. This assembles it with the state that makes it readable — which
 * link, which board, which Zcash network and server, how far the wallet has
 * synced — formatted for a chat box rather than as JSON, because the person
 * reading it is a human and the person sending it is on a phone.
 *
 * ---------------------------------------------------------------------------
 * What it must never contain, and why. This list is a decision, not an
 * oversight, and it is short because the app holds no seed, PIN or spending
 * key — those never leave the device (PROTOCOL.md 6). What is left is:
 *
 * - **The session passkey.** Excluded. It is the value the user compares
 *   against the device screen to prove the encrypted channel reaches *that*
 *   device; pasting it into a chat while the session is live hands it to
 *   whoever reads the chat. The log already says whether pairing worked.
 * - **The passphrase.** Excluded, and it has no path in here: only whether
 *   one is applied.
 * - **The viewing key and addresses.** Excluded. A UFVK reveals the whole
 *   history of the wallet, and an address links the report to its owner. The
 *   seed fingerprint (ZFP) is included: it names which wallet without
 *   revealing anything spendable or viewable, and "the wrong wallet opened"
 *   is a bug this report exists for.
 * - **Whatever is typed into the form fields** (a recipient, an amount, a
 *   memo). Excluded.
 * - **The wallet's data directory.** Excluded: on a desktop it carries the
 *   user's name.
 *
 * The user agent is included because "which Android" is the first question
 * anyone will ask.
 */
const when = (d: Date | null): string =>
  d === null ? "never this session" : `${d.toISOString()} (${Math.round((Date.now() - d.getTime()) / 1000)} s ago)`;

async function diagnosticsReport(): Promise<string> {
  const L: string[] = [];

  L.push("Zcash LeekWallet (the LeekWallet Zcash Companion) — diagnostics");
  L.push(new Date().toISOString());
  L.push(`User agent: ${navigator.userAgent}`);
  L.push("");

  L.push("== Connection");
  L.push(`State:      ${$("conn").textContent ?? "?"}`);
  L.push(`Backend:    ${$("mode").textContent ?? "?"}`);
  L.push(`Link:       ${LINK_NAMES[selectedKind()]} (selected)`);
  L.push(`Transports: ${available.length > 0 ? available.join(", ") : "none — mock only"}`);
  L.push(`Session:    ${client === null ? "none" : client.encrypted ? "encrypted" : "plaintext (not paired)"}`);
  L.push(
    `Device:     ${lastStatus.unlocked ? "unlocked" : "locked"}, ` +
      (lastStatus.temporary ? "temporary seed, " : `wallet ${lastStatus.activeWallet}/${lastStatus.walletCount}, `) +
      /* Whether one is applied, never a character of it. */
      `passphrase ${lastStatus.passphrase ? "on" : "off"}`,
  );
  L.push("");

  L.push("== Board");
  if (deviceFeatures === null) {
    L.push("Not read (no device connected).");
  } else {
    const f = deviceFeatures;
    const text = (k: string) => (typeof f[k] === "string" ? String(f[k]) : "?");
    L.push(`Firmware:   ${text("firmware")}, model ${text("model")}, chain ${text("chain")}`);
  }
  /* zGetFeatures is in the keys tier: only over a paired session, and asked
   * now rather than cached so the report says what the board answers. */
  if (client?.encrypted) {
    const c = client;
    let z: ZFeatures | null = null;
    try {
      z = await zGetFeatures((m, p, t) => c.call(m, p, t));
    } catch (e) {
      L.push(`Zcash features: not read: ${zErrorText(e)}`);
    }
    if (z !== null) {
      L.push(`Signer:     ${z.backend}, show address ${z.showAddress ? "yes" : "no"}`);
      L.push(`Branch IDs: ${z.branchIds.map((b) => `0x${b.toString(16)}`).join(", ") || "none"}`);
      L.push(`Limits:     PCZT ${z.maxPczt} B, chunk ${z.maxChunk} B`);
    }
  } else {
    L.push("Zcash features: not read (no paired session).");
  }
  /* The board answers for one network only (its own setting) and refuses a
   * request for the other (0x0528); newer firmware reports it in getStatus. */
  L.push(boardNetwork === null
    ? "Network setting: not reported by this firmware; it refuses requests for the other network (0x0528)."
    : `Network setting: ${networkLabel(boardNetwork)} (getStatus)`);
  const acct = zcashView?.boardAccount ?? null;
  L.push(
    acct === null
      ? "Account:    not read"
      : `Account:    ${acct.account} on ${networkLabel(acct.network)}, ZFP ${acct.seedFingerprint}, ` +
        `birthday ${acct.birthday ?? "not set on the board"}`,
  );
  L.push("");

  L.push("== Zcash");
  L.push(`Network:    ${zcashView ? networkLabel(zcashView.network) : "?"}, account ${zcashView?.account ?? "?"}`);
  const w = zcashWallet?.diagnostics() ?? null;
  if (w === null) {
    L.push("Wallet:     not in this build (board tools only), or not started.");
  } else {
    const s = w.status;
    if (s === null) {
      L.push("Wallet:     none open (the board's account is read first).");
    } else {
      L.push(`Server:     ${s.server}${s.server === s.defaultServer ? " (default)" : ` (default ${s.defaultServer})`}`);
      L.push(`Last contact: ${when(w.lastContact)}`);
      L.push(`Wallet:     ${s.hasAccount ? `account ${s.accountIndex ?? "?"}, ZFP ${s.zfp ?? "?"}` : "no wallet for this seed yet"}`);
      if (s.birthday !== null) L.push(`Birthday:   ${s.birthday}`);
      L.push(`Synced:     ${s.fullyScanned ?? "never"} of tip ${s.chainTip ?? "?"}`);
      L.push(`Board wallet: ${s.hardware ? "yes (signs on the board)" : "no"}`);
    }
    L.push(`Last sync:  ${w.lastSync === null ? "none this session" : `${w.lastSync.at.toISOString()} — ${w.lastSync.text}`}`);
  }
  L.push("");

  L.push("== Device log (newest first)");
  L.push(logText());

  return L.join("\n");
}

type CopyRoute = "tauri" | "async-clipboard" | "exec-command" | "manual";

/**
 * Put text on the clipboard, trying every route this app can reach.
 *
 * There are three because none of them works everywhere:
 *
 * 1. **Tauri's clipboard plugin**, if the native build has it. It is the only
 *    route that is unambiguously correct inside an Android webview. NOTE: as
 *    of this commit `src-tauri/capabilities/default.json` grants only
 *    `core:default`, so the plugin is *not* registered and this probe will
 *    miss; it is written first anyway so that adding the plugin on the Rust
 *    side is the only change needed to light it up.
 * 2. **`navigator.clipboard`**, which requires a secure context. A Tauri
 *    Android window is served from `http://tauri.localhost`, which is not one
 *    of the origins browsers treat as secure, so this may well be undefined
 *    there. It is the right answer on desktop and in a dev browser tab.
 * 3. **`document.execCommand("copy")`** over a hidden textarea. Deprecated,
 *    and still the thing that actually works in an Android webview under a
 *    user gesture — which a button click is.
 *
 * If all three fail the text is put on screen, selected, and the caller says
 * so. A copy button that silently does nothing is worse than no button: the
 * user pastes stale clipboard content and nobody finds out for an hour.
 */
async function copyText(text: string): Promise<CopyRoute> {
  const tauri = (window as unknown as {
    __TAURI__?: { clipboardManager?: { writeText?: (t: string) => Promise<void> } };
  }).__TAURI__?.clipboardManager?.writeText;
  if (tauri) {
    try {
      await tauri(text);
      return "tauri";
    } catch { /* fall through: a failed plugin call is not a reason to give up */ }
  }

  if (window.isSecureContext && navigator.clipboard?.writeText) {
    try {
      await navigator.clipboard.writeText(text);
      return "async-clipboard";
    } catch { /* permission denied or no gesture; try the old way */ }
  }

  /* The textarea has to be in the document, visible to the layout engine and
   * focused for the selection to be real, so it is placed off-screen rather
   * than hidden — `display: none` cannot be selected. */
  try {
    const ta = document.createElement("textarea");
    ta.value = text;
    ta.setAttribute("readonly", "");
    ta.style.cssText = "position:fixed;top:0;left:-9999px;opacity:0;";
    document.body.appendChild(ta);
    ta.select();
    ta.setSelectionRange(0, text.length);
    const ok = document.execCommand("copy");
    ta.remove();
    if (ok) return "exec-command";
  } catch { /* fall through to manual */ }

  const out = $("copyout") as HTMLTextAreaElement;
  out.value = text;
  out.hidden = false;
  out.focus();
  out.select();
  return "manual";
}

$("copydiag").addEventListener("click", () => {
  const out = $("copyout") as HTMLTextAreaElement;
  out.hidden = true;
  const status = $("copystatus");
  status.textContent = "Copying…";

  let text = "";
  void diagnosticsReport().then((t) => { text = t; return copyText(t); }).then(
    (route) => {
      const lines = text.split("\n").length;
      status.textContent =
        route === "manual"
          ? "This build could not reach the clipboard. The text is below and selected — copy it by hand."
          : `Copied ${lines} lines (${route}). No passkey, passphrase, viewing key, address or form input is included.`;
      log(`diagnostics copied via ${route}`);
    },
    (e: unknown) => {
      status.textContent = `Copy failed: ${(e as Error).message}`;
      log(`diagnostics copy failed: ${(e as Error).message}`);
    },
  );
});

$("theme").addEventListener("click", () => {
  const next = THEME_ORDER[(THEME_ORDER.indexOf(currentTheme()) + 1) % THEME_ORDER.length] ?? "system";
  localStorage.setItem(THEME_KEY, next);
  applyTheme(next);
  log(`theme: ${next}`);
});
