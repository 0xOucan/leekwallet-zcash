/**
 * In-process mock of the LeekWallet firmware.
 *
 * Implements the wire protocol from docs/PROTOCOL.md faithfully enough that the
 * companion app can be built, demoed and tested with no hardware attached — the
 * single highest-leverage thing for parallel work, since UI development stops
 * waiting on firmware.
 *
 * It is a protocol mock, not a wallet: it models the session, the PIN, the
 * wallet and passphrase state, and nothing that holds a key. The Zcash
 * methods (zGetAccount, zSign...) are not here: those are tested against
 * zdev, the firmware's own protocol on a PTY (test/zcash-zdev.test.ts).
 *
 * The parts modelled carefully are the ones that catch UI bugs: permission
 * tiers, the confirmation round-trip, user rejection, and the fact that the
 * device is slow. A mock that answers instantly hides every missing spinner.
 */

import { encodeCbor, decodeCbor, type CborValue } from "./cbor.ts";
import { encodeFrame, FrameDecoder, FrameType } from "./framing.ts";
import { ErrorCode, type Transport } from "./transport.ts";
import { PROTOCOL_VERSION } from "./session.ts";

export interface MockOptions {
  /** Milliseconds each command takes. Real key derivation costs ~1 s. */
  latencyMs?: number;
  /** Auto-approve confirmations. Set false to exercise the rejection path. */
  autoApprove?: boolean;
  /** Start already unlocked, skipping the PIN prompt. */
  startUnlocked?: boolean;

  /**
   * Whether the simulated user eventually types the PIN after `unlock`.
   *
   * `unlock` only prompts — the device answers `{prompted:1, unlocked:0}` and
   * the host polls `getStatus` (PROTOCOL.md 6e #1). Default true so the demo
   * app makes progress; set false to drive `enterPin()` by hand, which is the
   * only way to assert on the window where the device is prompting and still
   * locked.
   */
  autoPin?: boolean;

  /** How long the simulated user takes to type the PIN, when `autoPin`. */
  pinEntryMs?: number;
  /** How many seeds the device holds. */
  walletCount?: number;

  /**
   * Whether `helloReveal` completes the passkey comparison by itself.
   *
   * Default true, because pairing is not what most tests are about. Set false
   * to leave the session PENDING and drive `confirmSession()` yourself - that
   * is the path where a machine in the middle gets caught, so something should
   * exercise it.
   */
  autoConfirmSession?: boolean;

  /**
   * The `blindSigning` flag the firmware still reports in getFeatures (a
   * setting inherited from LeekWallet; nothing on the Zcash firmware reads
   * it). Here only so the conformance vectors that set it compare.
   */
  blindSigning?: boolean;
}

type Handler = (params: Record<string, CborValue>) => CborValue;

const DEVICE_LABEL = "LeekWallet (mock)";

/* Fixed so the pairing UI has something stable to render. A real device
 * derives this from the handshake; see session.ts. */
const MOCK_PASSKEY = "314159";

export class MockDevice implements Transport {
  readonly kind = "mock" as const;
  readonly label = DEVICE_LABEL;

  private opened = false;

  /* Four states, as session.c has them. The mock used to jump straight from
   * nothing to established on `hello`, which skipped the passkey comparison
   * entirely - the whole defence against a machine in the middle. Anything
   * built against that mock would pass without ever exercising it.
   * `awaitingReveal` is the v2 addition: the device has committed to its nonce
   * and nothing is derived or displayed until the host reveals its own. */
  private sessionState: "none" | "awaitingReveal" | "pending" | "active" = "none";
  private unlocked: boolean;
  /** The PIN pad is on screen and the device is waiting for the user. */
  private pinPrompted = false;
  private handler: ((frame: Uint8Array) => void) | null = null;
  private readonly decoder = new FrameDecoder();
  private readonly opts: Required<MockOptions>;

  private activeWallet = 1;
  private passphraseActive = false;

  /** Requests the device is "showing" — inspect in tests. */
  readonly confirmations: string[] = [];

  /** The user confirmed the passkey matches. Nothing encrypted works before. */
  confirmSession(): void {
    if (this.sessionState === "pending") this.sessionState = "active";
  }

  /** Session state, for tests that care about the pending step. */
  get session(): "none" | "awaitingReveal" | "pending" | "active" {
    return this.sessionState;
  }

  /** Test hook: simulate the device auto-locking on its idle timer. */
  autoLock(): void {
    this.unlocked = false;
    this.passphraseActive = false;
    this.pinPrompted = false;
  }

  /**
   * Test hook: the user finished typing the PIN on the device keypad.
   *
   * Nothing crosses the wire when this happens — the host only learns about it
   * by polling `getStatus`, which is exactly the behaviour app code has to be
   * built against.
   */
  enterPin(): void {
    if (this.pinPrompted) {
      this.pinPrompted = false;
      this.unlocked = true;
    }
  }

  /** Whether the device is currently showing its PIN pad. */
  get promptingForPin(): boolean {
    return this.pinPrompted;
  }

  constructor(options: MockOptions = {}) {
    this.opts = {
      latencyMs: options.latencyMs ?? 0,
      autoApprove: options.autoApprove ?? true,
      startUnlocked: options.startUnlocked ?? false,
      walletCount: options.walletCount ?? 1,
      autoConfirmSession: options.autoConfirmSession ?? true,
      autoPin: options.autoPin ?? true,
      pinEntryMs: options.pinEntryMs ?? 400,
      blindSigning: options.blindSigning ?? false,
    };
    this.unlocked = this.opts.startUnlocked;
  }

  /**
   * The BIP44 account the simulated device's own screens are on.
   *
   * Settable so a test can do what a user does with the button: change it
   * behind the app's back and check the app notices.
   */
  hdAccount = 0;

  get isOpen(): boolean {
    return this.opened;
  }

  async open(): Promise<void> {
    this.opened = true;
    this.sessionState = "none";
    this.decoder.reset();
  }

  async close(): Promise<void> {
    this.opened = false;
    this.sessionState = "none";
    // Disconnect clears session secrets, as the firmware does.
    this.passphraseActive = false;
  }

  onFrame(handler: (frame: Uint8Array) => void): void {
    this.handler = handler;
  }

  async send(frame: Uint8Array): Promise<void> {
    if (!this.opened) throw new Error("transport is not open");

    for (const decoded of this.decoder.push(frame)) {
      const reply = this.dispatch(decoded.type, decoded.payload);
      if (this.opts.latencyMs > 0) {
        await new Promise((r) => setTimeout(r, this.opts.latencyMs));
      }
      this.handler?.(reply);
    }
  }

  /* ------------------------------------------------------------ dispatch */

  private dispatch(type: number, payload: Uint8Array): Uint8Array {
    let request: CborValue;
    try {
      request = decodeCbor(payload);
    } catch {
      return this.error(ErrorCode.MalformedFrame, "undecodable CBOR");
    }

    // Uint8Array is also `typeof "object"`, so exclude it explicitly or the
    // map access below is unsound.
    if (
      typeof request !== "object" ||
      request === null ||
      Array.isArray(request) ||
      request instanceof Uint8Array
    ) {
      return this.error(ErrorCode.MalformedFrame, "request must be a map");
    }

    const map = request as Record<string, CborValue>;
    const method = map["method"];
    if (typeof method !== "string") {
      return this.error(ErrorCode.MalformedFrame, "missing method");
    }
    /* Fields are top-level, beside `method` - the same shape the firmware
     * parses. Reading them from a nested `params` object would let the mock
     * accept requests real hardware ignores. */
    const params = map;

    /* The "always" tier from PROTOCOL.md section 4: answerable with no session
     * because they reveal nothing. `getStatus` belongs here - section 5 tells
     * the app to poll it, and the firmware answers it in plaintext - and
     * `ping` exists on the device and was simply missing here. */
    const preSession =
      method === "hello" || method === "helloReveal" ||
      method === "getFeatures" || method === "getStatus" || method === "ping";

    /* A method that does not exist is malformed BEFORE it is unauthorised, and
     * that order is the firmware's rather than the tidier-looking one: dispatch
     * in protocol.c matches the name first and only the handlers it found check
     * the session. So `getMnemonic` with no session is 0x0001 on the device and
     * was 0x0400 here — the mock telling a host that a method it does not have
     * exists and merely needs pairing. */
    const handler = this.handlers[method];
    if (!handler) {
      return this.error(ErrorCode.MalformedFrame, `unknown method ${method}`);
    }

    /* PENDING is not established. Until the user has compared the passkey,
     * the device refuses everything else, and so must this. */
    if (!preSession && this.sessionState !== "active") {
      return this.error(ErrorCode.SessionRequired, "no session");
    }

    try {
      return this.ok(handler(params), type === FrameType.Request);
    } catch (e) {
      if (e instanceof MockRejection) return this.error(e.code, e.message);
      throw e;
    }
  }

  private requireUnlocked(): void {
    if (!this.unlocked) throw new MockRejection(ErrorCode.NotUnlocked, "device is locked");
  }

  /** Model the confirm-on-device round trip, including refusal. */
  private confirm(description: string): void {
    this.confirmations.push(description);
    if (!this.opts.autoApprove) {
      throw new MockRejection(ErrorCode.UserRejected, "rejected on device");
    }
  }

  private readonly handlers: Record<string, Handler> = {
    /* Two legs, as PROTOCOL.md §3 and src/protocol.c have them. The mock has
     * no key agreement, so it cannot produce a real public key, commitment or
     * nonce — but it can and must model the STATE MACHINE, because the ordering
     * is the security property. A mock that established a session from `hello`
     * alone would let host code skip the reveal and the commitment check and
     * still pass every test here, which is precisely how the mock has twice
     * certified behaviour the firmware refuses. */
    hello: (params) => {
      if (params["version"] !== PROTOCOL_VERSION) {
        throw new MockRejection(
          ErrorCode.UnsupportedVersion,
          `this device speaks protocol v${PROTOCOL_VERSION}; the app offered ` +
          `v${typeof params["version"] === "number" ? params["version"] : "none"}`,
        );
      }
      this.sessionState = "awaitingReveal";
      return { version: PROTOCOL_VERSION, deviceId: "mock-0001" };
    },

    helloReveal: () => {
      if (this.sessionState !== "awaitingReveal") {
        throw new MockRejection(
          ErrorCode.SessionRequired, "no handshake is waiting for a nonce",
        );
      }
      /* PENDING, not established. The device shows a passkey and waits for the
       * user to confirm it matches; nothing encrypted is accepted until then. */
      this.sessionState = "pending";
      this.confirmations.push(`Compare passkey ${MOCK_PASSKEY}`);
      if (this.opts.autoConfirmSession) {
        this.sessionState = "active";
      }
      return { passkey: MOCK_PASSKEY };
    },

    ping: () => ({ pong: 1 }),

    /* Exactly the fields protocol.c writes. `initialized` was invented
     * here and existed nowhere else, so app code could branch on a field real
     * hardware never sends. */
    getFeatures: () => ({
      blindSigning: this.opts.blindSigning ? 1 : 0,
      chain: "zcash",
      firmware: "0.1.0-mock",
      model: "LeekWallet-mock",
    }),

    getStatus: () => ({
      unlocked: this.unlocked ? 1 : 0,
      walletCount: this.opts.walletCount,
      activeWallet: this.activeWallet,
      /* The mock has no temporary mode -- it is a device-side flow with no
       * host half -- but the field has to be here, because the app decides
       * what to render from it and a mock that omits it certifies an app that
       * would show `wallet 0/N` against real firmware. */
      temporary: 0,
      passphrase: this.passphraseActive ? 1 : 0,
      /* Reported like the real device, so the mock cannot certify an app that
       * ignores the field -- the failure this suite exists to catch. */
      account: this.hdAccount,
    }),

    /**
     * Prompt, and say nothing about the outcome.
     *
     * The PIN is typed on the device, so the reply cannot carry the result —
     * it is sent long before the user has touched a button. The device answers
     * `{prompted:1, unlocked:0}` and the host polls `getStatus`. The mock used
     * to unlock synchronously and return `{unlocked:1}`, which taught every app
     * built on it that unlocking is instantaneous.
     *
     * Already unlocked is the one case with an immediate answer, and protocol.c
     * takes it: no prompt, `{unlocked:1}`.
     */
    unlock: () => {
      if (this.unlocked) return { unlocked: 1 };

      /* Recorded, not confirm()ed: a PIN pad is not an approve/reject screen,
       * and `autoApprove` governs signing decisions. Whether the user types the
       * PIN is `autoPin`. */
      this.confirmations.push("Enter PIN on device");
      this.pinPrompted = true;
      if (this.opts.autoPin) {
        setTimeout(() => this.enterPin(), this.opts.pinEntryMs);
      }
      return { prompted: 1, unlocked: 0 };
    },

    lock: () => {
      this.unlocked = false;
      this.pinPrompted = false;
      /* The passphrase dies with the session, as it does on the device. A host
       * that kept deriving addresses from it would be showing a wallet the
       * device can no longer produce. */
      this.passphraseActive = false;
      // `{unlocked:0}`, as protocol.c answers - an empty map told the host
      // nothing about the state it had just changed.
      return { unlocked: 0 };
    },

    selectWallet: (p) => {
      this.requireUnlocked();
      /* No index is a malformed request, not wallet 1. Defaulting made the
       * mock answer `{activeWallet:1}` where the device answers 0x0001, and a
       * host that lost the field would look correct here and select nothing
       * there. */
      const index = p["index"];
      if (typeof index !== "number" || !Number.isInteger(index)) {
        throw new MockRejection(ErrorCode.MalformedFrame, "index required");
      }
      if (index < 1 || index > this.opts.walletCount) {
        throw new MockRejection(ErrorCode.NoWallet, `no wallet ${index}`);
      }
      this.activeWallet = index;
      // Switching seeds drops the passphrase, as the firmware does.
      this.passphraseActive = false;
      return { activeWallet: index };
    },

    setPassphrase: (p) => {
      this.requireUnlocked();

      /* The passphrase was not looked at here at all: any request applied one,
       * including an empty one and one the device's own keyboard cannot type.
       * protocol.c bounds it at 1..63 printable-ASCII bytes and refuses the
       * rest as malformed — an empty string is the base wallet, not a
       * passphrase, and a wallet reachable from the app but not from the device
       * is one the owner cannot get back to without the app. */
      const raw = p["passphrase"];
      if (typeof raw !== "string") {
        throw new MockRejection(ErrorCode.MalformedFrame, "passphrase required");
      }
      if (raw.length === 0 || raw.length > PASSPHRASE_MAX_BYTES) {
        throw new MockRejection(ErrorCode.MalformedFrame, "passphrase length out of range");
      }
      if (!/^[\x20-\x7e]+$/.test(raw)) {
        throw new MockRejection(ErrorCode.MalformedFrame, "passphrase must be printable ASCII");
      }

      /* The device applies it, derives the resulting seed and asks the owner
       * to recognise its fingerprint, which is what the reply carries: the
       * seed is the question that screen asks, the same for every account
       * and network (protocol.c). */
      const fingerprint = mockFingerprint(this.activeWallet, true);
      this.confirm(`Confirm passphrase seed ${fingerprint.toString(16).padStart(8, "0")} on device`);
      /* Set only after the confirmation: a rejection must leave the device in
       * the wallet it was already in, or the owner ends up signing from one
       * they refused. confirm() throws on refusal, so this line is the
       * approval. */
      this.passphraseActive = true;
      return { fingerprint, passphrase: 1 };
    },
  };

  /* -------------------------------------------------------------- framing */

  private ok(result: CborValue, plaintext: boolean): Uint8Array {
    return encodeFrame(
      plaintext ? FrameType.Response : FrameType.EncryptedResponse,
      encodeCbor({ result }),
    );
  }

  private error(code: number, message: string): Uint8Array {
    /* Once a session is active an error is encrypted like any other reply, and
     * carries its own frame type.
     *
     * Not for secrecy - for counters. The device advances its receive counter
     * the moment a frame decrypts, error or not, while the host only advances
     * on opening a reply. A plaintext error leaves the two one apart and every
     * later frame fails to decrypt. The firmware learned this on hardware; the
     * mock kept answering in plaintext, so a client that mishandled it passed
     * here and desynced against the real device. */
    const type =
      this.sessionState === "active" ? FrameType.EncryptedError : FrameType.Error;
    return encodeFrame(type, encodeCbor({ code, message }));
  }
}

class MockRejection extends Error {
  readonly code: number;
  constructor(code: number, message: string) {
    super(message);
    this.code = code;
  }
}

/** Longest passphrase protocol.c will accept: 63 bytes plus its terminator. */
const PASSPHRASE_MAX_BYTES = 63;

/**
 * Deterministic stand-in for a seed fingerprint (a uint32, as the device
 * reports it). NOT a real one: it exists so that changing wallet or
 * passphrase visibly changes the answer the way a real device would.
 */
function mockFingerprint(wallet: number, passphrase: boolean): number {
  const seed = `${wallet}|${passphrase ? "pp" : "no"}`;
  let h = 0x811c9dc5;
  for (let i = 0; i < seed.length; i++) {
    h ^= seed.charCodeAt(i);
    h = Math.imul(h, 0x01000193) >>> 0;
  }
  return h;
}
