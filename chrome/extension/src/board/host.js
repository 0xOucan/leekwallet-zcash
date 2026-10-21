// The board, as the wallet uses it: connect and pair, wait for the PIN,
// import the board's account into the engine, check an address on the
// board's screen, and sign a proposal there and finish it in the engine.
//
// Context-free: it is given a way to open a link and a way to call the
// engine. The offscreen document runs one for USB (Web Serial), calling its
// engine worker directly; an extension tab runs one for Bluetooth (the
// BluetoothDevice cannot leave the tab that chose it), calling the engine
// through EngineClient. Both publish the same state to the popup.
//
//   const host = new BoardHost({ openLink, engine, onState });
//   await host.run('connect', { kind: 'usb', network: 'test' });
//   await host.run('importAccount', { network: 'test' });
//   await host.run('signAndFinish', { network, accountIndex, signerView, broadcast: true });
//   await host.run('setPassphrase', { passphrase, network });   // typed here: the weaker path
//
// Which wallet the board holds can change under the host: the user enters
// a passphrase on the board (the safer path), the board locks, or another
// wallet is selected there. So while connected and idle the host asks
// `getStatus` every `watchMs`, and when what it says changes (unlocked,
// passphrase, active wallet) it reads the account's seed fingerprint again.

import { Device, DeviceError, ERR, USER_TIMEOUT, fromHex, toHex } from './device.js';
import { encodeZsig } from './zsig.js';

/** An error the popup acts on (it carries what to ask the user next). */
export class BoardNeeds extends Error {
  constructor(need, message, extra = {}) {
    super(message);
    this.name = 'BoardNeeds';
    this.need = need;
    Object.assign(this, extra);
  }
}

/** The longest a link may take to close before the host goes on without it. */
export const LINK_CLOSE_MS = 5000;

function closeLink(link) {
  let timer;
  return Promise.race([
    Promise.resolve()
      .then(() => link.close())
      .catch(() => {}),
    new Promise((r) => (timer = setTimeout(r, LINK_CLOSE_MS))),
  ]).finally(() => clearTimeout(timer));
}

const OPS = new Set(['connect', 'disconnect', 'cancel', 'state', 'account', 'importAccount', 'showAddress', 'signAndFinish', 'setPassphrase', 'checkWallet']);

/** How often a connected, idle board is asked whether its wallet changed. */
export const WATCH_MS = 4000;

/**
 * The firmware's rule for a passphrase (protocol.c, setPassphrase): 1 to 63
 * bytes of printable ASCII, the set the board's own keyboard can type. An
 * error message, or null when it may be sent. Never echoes the passphrase.
 */
export function passphraseComplaint(value) {
  if (typeof value !== 'string' || value.length === 0) return 'Nothing to send. An empty passphrase is the wallet without one, not a passphrase.';
  if (new TextEncoder().encode(value).length > 63) return 'Too long: the board accepts up to 63 characters.';
  for (const ch of value) {
    const c = ch.codePointAt(0);
    if (c < 0x20 || c > 0x7e) return "Only printable ASCII, the characters the board's own keyboard can type.";
  }
  return null;
}

export class BoardHost {
  #openLink;
  #engine;
  #onState;
  #device = null;
  #busy = null;
  #abort = null;
  #linkClosing = null;
  #network = 'test';
  #watchMs;
  #watchTimer = null;
  #watching = null;
  /** The board's last `getStatus`: { unlocked, passphrase, activeWallet }. */
  #status = null;
  /**
   * What the popup draws. Plain JSON: it crosses runtime ports.
   * `seedFingerprint` is the ZIP 32 fingerprint of the seed the board has
   * unlocked, read once connected: the popup shows a device account only
   * while the board that holds its seed is connected.
   * `passphrase`: the board says a passphrase is applied (a hidden wallet,
   * entered on the board or from here); `xfp`: the fingerprint the board
   * showed for a passphrase sent from here. `locked`: the board locked
   * while connected (no wallet is shown until it is unlocked again).
   */
  state = { phase: 'disconnected', kind: null, label: null, passkey: null, waiting: null, task: null, error: null, seedFingerprint: null, passphrase: false, xfp: null, locked: false, lastResult: null };

  constructor({ openLink, engine, onState, watchMs = WATCH_MS }) {
    this.#openLink = openLink;
    this.#engine = engine;
    this.#onState = onState ?? (() => {});
    this.#watchMs = watchMs;
  }

  /** The operation running now, or null. */
  get busy() {
    return this.#busy;
  }

  #set(patch) {
    this.state = { ...this.state, ...patch };
    this.#onState({ ...this.state });
  }

  /** Runs one operation; one at a time, except `cancel` and `state`. */
  async run(op, params = {}) {
    if (!OPS.has(op)) throw new Error(`unknown board operation ${op}`);
    if (op === 'state') return { ...this.state };
    if (op === 'cancel') {
      this.#abort?.abort();
      return { cancelling: !!this.#busy };
    }
    if (op === 'disconnect') {
      this.#abort?.abort();
      await this.#close('disconnected');
      return { ...this.state };
    }
    // A status check in flight is short: wait for it rather than refuse.
    if (this.#watching) await this.#watching.catch(() => {});
    if (this.#busy) throw new Error(`the board is busy (${this.#busy})`);
    this.#busy = op;
    if (this.state.error) this.#set({ error: null });
    this.#abort = new AbortController();
    try {
      return await this[`op_${op}`](params, this.#abort.signal);
    } catch (e) {
      if (!(e instanceof BoardNeeds)) this.#set({ error: String(e?.message ?? e) });
      // A session that can no longer line up (a timeout, a dropped link)
      // is closed, so the popup offers to connect again.
      if (this.#device && !this.#device.paired && op !== 'connect') await this.#close(String(e?.message ?? e));
      throw e;
    } finally {
      this.#busy = null;
      this.#abort = null;
      if (this.state.task) this.#set({ task: null });
    }
  }

  /**
   * Ends the session. The state goes first, so every page hides the
   * board's wallet at once, whatever the link does next; then the link is
   * closed (the serial port released, the GATT connection dropped), given
   * LINK_CLOSE_MS at most: a Bluetooth stack that never answers must not
   * keep the wallet on screen nor the next connect waiting.
   */
  async #close(why) {
    const d = this.#device;
    this.#device = null;
    this.#stopWatch();
    this.#status = null;
    // The board drops a passphrase sent from here when the session ends;
    // this side forgets everything about it too.
    this.#set({ phase: 'disconnected', passkey: null, waiting: null, task: null, seedFingerprint: null, passphrase: false, xfp: null, locked: false, accountError: null, error: why === 'disconnected' ? null : why });
    // A close already under way (the link failed by itself) is waited for too.
    if (d) this.#linkClosing = closeLink(d.link);
    await this.#linkClosing;
  }

  #ready() {
    if (!this.#device?.paired) throw new BoardNeeds('connect', 'Connect the board first.');
    return this.#device;
  }

  async #checkNetwork(network) {
    const s = await this.#engine('status', {});
    if (!s.open || s.network !== network) throw new Error(`the wallet open in the engine is ${s.network ?? 'none'}, not ${network}`);
    return s;
  }

  async op_connect({ kind, network = 'test' }, signal) {
    if (this.#device) await this.#close('disconnected');
    this.#network = network;
    this.#set({ phase: 'connecting', kind, label: null, passkey: null, waiting: null, seedFingerprint: null, passphrase: false, xfp: null, locked: false, accountError: null, error: null });
    let link;
    try {
      link = await this.#openLink(kind, (why) => {
        if (this.#device?.link === link) void this.#close(why);
      });
    } catch (e) {
      this.#set({ phase: 'disconnected' });
      throw e;
    }
    // Disconnected (or cancelled) while the link opened: it is not kept.
    if (signal.aborted) {
      await closeLink(link);
      this.#set({ phase: 'disconnected' });
      throw new Error('cancelled');
    }
    const device = new Device(link);
    this.#device = device;
    this.#set({ label: link.label });
    try {
      await device.pair({
        signal,
        onBusy: () => this.#set({ waiting: 'busy' }),
        onPasskey: (passkey) => this.#set({ phase: 'pairing', passkey, waiting: 'allow' }),
      });
      this.#set({ phase: 'unlocking', waiting: null });
      await device.ensureUnlocked({ signal, onWaiting: () => this.#set({ waiting: 'pin' }) });
      const features = await device.zGetFeatures().catch(() => null);
      this.#status = await this.#readStatus(device).catch(() => null);
      // Which wallet this is. The fingerprint is the seed's, the same on
      // both networks; deriving the account takes the board a few seconds.
      this.#set({ waiting: 'wallet' });
      const wallet = await this.#readWallet(device, network);
      this.#set({ phase: 'ready', passkey: null, waiting: null, features, passphrase: !!this.#status?.passphrase, locked: false, ...wallet });
      this.#startWatch();
      return { ...this.state };
    } catch (e) {
      await this.#close(String(e?.message ?? e));
      throw e;
    }
  }

  /** getStatus, the fields that say which wallet is open. */
  async #readStatus(device) {
    const s = await device.call('getStatus');
    return { unlocked: s?.unlocked === 1, passphrase: s?.passphrase === 1, activeWallet: Number(s?.activeWallet ?? 0) };
  }

  /**
   * The seed fingerprint of the board's wallet now. Without it (an error
   * on the board) no device account is shown, and the popup says why (a
   * board set to the other network, say).
   */
  async #readWallet(device, network) {
    let accountError = null;
    const acc = await device.zGetAccount(0, network).catch((e) => ((accountError = String(e?.message ?? e)), null));
    return { seedFingerprint: acc?.seedFingerprint ?? null, accountError };
  }

  #startWatch() {
    this.#stopWatch();
    if (!this.#watchMs) return;
    this.#watchTimer = setInterval(() => {
      if (this.#busy || this.#watching || !this.#device?.paired) return;
      this.#watching = this.#check().finally(() => (this.#watching = null));
      this.#watching.catch(() => {});
    }, this.#watchMs);
  }

  #stopWatch() {
    clearInterval(this.#watchTimer);
    this.#watchTimer = null;
  }

  /**
   * Did the board's wallet change (a passphrase entered or dropped on the
   * board, a lock, another wallet selected)? If so, its fingerprint is read
   * again: a hidden wallet is another account, and its accounts show only
   * while the fingerprint matches. A session the board dropped (it locked,
   * say) ends here, hiding the wallet.
   */
  async #check({ force = false } = {}) {
    const device = this.#device;
    if (!device?.paired) return { ...this.state };
    let status;
    try {
      status = await this.#readStatus(device);
    } catch (e) {
      if (this.#device === device && !device.paired) await this.#close(String(e?.message ?? e));
      throw e;
    }
    const before = this.#status;
    this.#status = status;
    if (!status.unlocked) {
      // Locked on the board: it dropped any passphrase, and what it holds
      // is unknown until it is unlocked again.
      if (!this.state.locked || this.state.seedFingerprint) this.#set({ locked: true, seedFingerprint: null, passphrase: false, xfp: null });
      return { ...this.state };
    }
    const changed = force || !before || this.state.locked || before.passphrase !== status.passphrase || before.activeWallet !== status.activeWallet || before.unlocked !== status.unlocked;
    if (!changed) return { ...this.state };
    // Hidden at once: the wallet shown belonged to the state before.
    const xfp = status.passphrase && before?.passphrase && !force ? this.state.xfp : null;
    this.#set({ locked: false, seedFingerprint: null, passphrase: status.passphrase, xfp, waiting: 'wallet' });
    const wallet = await this.#readWallet(device, this.#network);
    if (this.#device === device) this.#set({ ...wallet, waiting: null });
    return { ...this.state };
  }

  /** Asks the board now whether its wallet changed (the popup's Check, and tests). */
  async op_checkWallet({ network } = {}) {
    this.#ready();
    if (network) this.#network = network;
    return this.#check();
  }

  /**
   * The weaker path to a hidden wallet: the passphrase typed in this
   * browser, sent over the encrypted session. The board derives the seed,
   * shows its fingerprint (XFP) and applies it only if the user confirms
   * there; a refusal or a timeout leaves the wallet it was in. Nothing here
   * keeps the passphrase: it is not in the state, the error, or any log.
   */
  async op_setPassphrase({ passphrase, network }) {
    const device = this.#ready();
    if (network) this.#network = network;
    const complaint = passphraseComplaint(passphrase);
    if (complaint) throw new Error(complaint);
    this.#set({ task: { op: 'passphrase', phase: 'confirm' } });
    let r;
    try {
      r = await device.call('setPassphrase', { passphrase }, USER_TIMEOUT);
    } catch (e) {
      const why =
        e instanceof DeviceError && e.code === ERR.USER_REJECTED
          ? 'rejected on the board'
          : e instanceof DeviceError && e.code === ERR.USER_TIMEOUT
            ? 'no answer on the board in time'
            : String(e?.message ?? e);
      throw new Error(`Passphrase not applied: ${why}. The board kept the wallet it was in.`);
    }
    const xfp = Number.isInteger(r?.fingerprint) ? r.fingerprint.toString(16).toUpperCase().padStart(8, '0') : null;
    // Another wallet now: hide the one shown, then read the new one.
    this.#set({ seedFingerprint: null, passphrase: true, xfp, locked: false, task: { op: 'passphrase', phase: 'wallet' }, waiting: 'wallet' });
    this.#status = await this.#readStatus(device).catch(() => null);
    const wallet = await this.#readWallet(device, this.#network);
    this.#set({ ...wallet, waiting: null });
    return { xfp, seedFingerprint: this.state.seedFingerprint };
  }

  async op_account({ network, accountIndex = 0 }) {
    const a = await this.#ready().zGetAccount(accountIndex, network);
    return a;
  }

  /**
   * zGetAccount -> the engine's importAccount as a device account, with the
   * board's birthday for this network; without one, `birthday` (asked from
   * the user) is used and recorded on the board.
   */
  async op_importAccount({ network, accountIndex = 0, birthday = null, name = null }) {
    const device = this.#ready();
    await this.#checkNetwork(network);
    this.#set({ task: { op: 'importAccount', phase: 'account' } });
    const acc = await device.zGetAccount(accountIndex, network);
    if (acc.seedFingerprint !== this.state.seedFingerprint) this.#set({ seedFingerprint: acc.seedFingerprint });
    let height = acc.birthday;
    if (height == null) {
      if (birthday == null || !(Number(birthday) > 0)) {
        const tip = await this.#engine('tip', {}).catch(() => null);
        throw new BoardNeeds('birthday', 'The board has no birthday for this account on this network: enter the height of its first transaction (or a little before).', { tip: tip?.height ?? null, account: acc });
      }
      height = Number(birthday);
    }
    this.#set({ task: { op: 'importAccount', phase: 'import' } });
    const r = await this.#engine('importAccount', {
      ufvk: acc.ufvk,
      birthday: height,
      name: name ?? `LeekWallet #${accountIndex}`,
      origin: { seedFingerprint: acc.seedFingerprint, accountIndex },
    });
    let storedOnBoard = null;
    if (acc.birthday == null) storedOnBoard = await device.zSetBirthday(accountIndex, network, height);
    return { ...r, board: acc, birthday: height, birthdayFromBoard: acc.birthday != null, storedOnBoard };
  }

  /** The board shows `ua` (any of the account's addresses) and the user confirms it. */
  async op_showAddress({ network, accountIndex = 0, ua }) {
    const device = this.#ready();
    this.#set({ task: { op: 'showAddress', phase: 'review' } });
    return device.zShowAddress(accountIndex, network, ua);
  }

  /**
   * Upload the signer view, review and sign on the board, then the engine's
   * finish: apply the .zsig, prove, extract, and broadcast if asked.
   * Mainnet needs `confirmMainnet: true`, the popup's real-funds tick.
   */
  async op_signAndFinish({ network, accountIndex = 0, signerView, broadcast = true, confirmMainnet = false }) {
    const device = this.#ready();
    if (network === 'main' && confirmMainnet !== true) {
      throw new Error('This moves real ZEC on mainnet: confirm it first.');
    }
    await this.#checkNetwork(network);
    const view = fromHex(signerView);
    this.#set({ task: { op: 'sign', phase: 'upload', sent: 0, total: view.length } });
    const signed = await device.zSign(view, accountIndex, network, (p) => this.#set({ task: { op: 'sign', ...p, total: p.total ?? view.length } }));
    const zsig = toHex(encodeZsig(signed.sha256, signed.entries));
    this.#set({ task: { op: 'sign', phase: 'finish', signatures: signed.entries.length } });
    const fin = await this.#engine('finish', { signerView, zsig, broadcast });
    // Kept with the wallet that signed it: the popup shows it only to that board.
    const result = { txid: fin.txid, broadcast: fin.broadcast, size: fin.size, proveMs: fin.proveMs, sighash: toHex(signed.sighash), signatures: signed.entries.length, network, seedFingerprint: this.state.seedFingerprint, accountIndex, at: Date.now() };
    this.#set({ lastResult: result });
    return { ...result, zsig };
  }
}
