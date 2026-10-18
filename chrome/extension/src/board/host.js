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

import { Device, fromHex, toHex } from './device.js';
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

const OPS = new Set(['connect', 'disconnect', 'cancel', 'state', 'account', 'importAccount', 'showAddress', 'signAndFinish']);

export class BoardHost {
  #openLink;
  #engine;
  #onState;
  #device = null;
  #busy = null;
  #abort = null;
  /**
   * What the popup draws. Plain JSON: it crosses runtime ports.
   * `seedFingerprint` is the ZIP 32 fingerprint of the seed the board has
   * unlocked, read once connected: the popup shows a device account only
   * while the board that holds its seed is connected.
   */
  state = { phase: 'disconnected', kind: null, label: null, passkey: null, waiting: null, task: null, error: null, seedFingerprint: null, lastResult: null };

  constructor({ openLink, engine, onState }) {
    this.#openLink = openLink;
    this.#engine = engine;
    this.#onState = onState ?? (() => {});
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

  async #close(why) {
    const d = this.#device;
    this.#device = null;
    await d?.link.close().catch(() => {});
    this.#set({ phase: 'disconnected', passkey: null, waiting: null, task: null, seedFingerprint: null, accountError: null, error: why === 'disconnected' ? null : why });
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
    this.#set({ phase: 'connecting', kind, label: null, passkey: null, waiting: null, seedFingerprint: null, accountError: null, error: null });
    let link;
    try {
      link = await this.#openLink(kind, (why) => {
        if (this.#device?.link === link) void this.#close(why);
      });
    } catch (e) {
      this.#set({ phase: 'disconnected' });
      throw e;
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
      // Which wallet this is. The fingerprint is the seed's, the same on
      // both networks; deriving the account takes the board a few seconds.
      this.#set({ waiting: 'wallet' });
      // Without it (an error on the board) no device account is shown,
      // and the popup says why (a board set to the other network, say).
      let accountError = null;
      const acc = await device.zGetAccount(0, network).catch((e) => ((accountError = String(e?.message ?? e)), null));
      this.#set({ phase: 'ready', passkey: null, waiting: null, features, seedFingerprint: acc?.seedFingerprint ?? null, accountError });
      return { ...this.state };
    } catch (e) {
      await this.#close(String(e?.message ?? e));
      throw e;
    }
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
