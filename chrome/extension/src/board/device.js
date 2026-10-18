// The board's protocol over any link: the v2 pairing handshake, the
// encrypted session, and the Zcash calls. The JavaScript twin of
// host/leek-device/src/device.rs (zecz, the desktop app and Android use
// that one), call for call, with the same timeouts and the same checks.
// firmware/docs/PROTOCOL.md is the specification.

import { decodeCbor, encodeCbor } from './cbor.js';
import { FRAME } from './framing.js';
import { LinkTimeout } from './link.js';
import { PROTOCOL_VERSION, NONCE_BYTES, Session, commitment, deriveKeys, generateKeyPair, randomBytes, sha256, sharedSecret } from './session.js';
import { POOL } from './zsig.js';

export const ERR = {
  NOT_UNLOCKED: 0x0100,
  USER_REJECTED: 0x0200,
  USER_TIMEOUT: 0x0201,
  SESSION: 0x0400,
  BUSY: 0x0401,
  ZSIGN_BASE: 0x0500,
  /** The request named the other network than the board's Settings > Network. */
  BOARD_NETWORK: 0x0500 | 40,
};

/** What to tell the user when the board refused `asked` ("main" or "test") because it is set to the other network. */
export function wrongNetworkMessage(asked) {
  const [app, board] = asked === 'main' ? ['Mainnet', 'Testnet'] : ['Testnet', 'Mainnet'];
  return `The board is set to ${board}, but this wallet is on ${app}. Switch the board to ${app} in Settings → Network, or switch this wallet to ${board}.`;
}

/** Replies can take as long as a person reading the review pages. */
export const USER_TIMEOUT = 150e3;
const SHORT = 10e3;
/** Calls that derive keys on the board: an Orchard account takes seconds at 160 MHz. */
const DERIVE = 90e3;

export class DeviceError extends Error {
  constructor(code, message) {
    const what =
      (code & 0xff00) === ERR.ZSIGN_BASE && code !== ERR.BOARD_NETWORK
        ? `the board refused to sign: ${message} (code -${code & 0xff})`
        : `board error 0x${code.toString(16).padStart(4, '0')}: ${message}`;
    super(what);
    this.name = 'DeviceError';
    this.code = code;
    this.deviceMessage = message;
  }
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

export const toHex = (b) => Array.from(b, (x) => x.toString(16).padStart(2, '0')).join('');
export function fromHex(s) {
  if (typeof s !== 'string' || s.length % 2 || /[^0-9a-f]/i.test(s)) throw new Error('not a hex string');
  const out = new Uint8Array(s.length / 2);
  for (let i = 0; i < out.length; i++) out[i] = parseInt(s.slice(2 * i, 2 * i + 2), 16);
  return out;
}

/** { result } -> result; an error frame -> DeviceError. */
function decodeReply(type, body) {
  let v;
  try {
    v = decodeCbor(body);
  } catch (e) {
    throw new Error(`the board's reply is not CBOR: ${e.message}`);
  }
  if (type === FRAME.ERROR || type === FRAME.ENC_ERROR) {
    throw new DeviceError(Number(v?.code ?? 0), String(v?.message ?? ''));
  }
  if (!v || typeof v !== 'object' || !('result' in v)) throw new Error('the board reply has no result');
  return v.result;
}

function bytesField(v, key, len) {
  const b = v?.[key];
  if (!(b instanceof Uint8Array)) throw new Error(`the board reply has no byte string "${key}"`);
  if (len !== undefined && b.length !== len) throw new Error(`"${key}" is ${b.length} bytes, expected ${len}`);
  return b;
}

function uintField(v, key) {
  const n = v?.[key];
  if (!Number.isInteger(n) || n < 0) throw new Error(`the board reply has no integer "${key}"`);
  return n;
}

function textField(v, key) {
  const t = v?.[key];
  if (typeof t !== 'string') throw new Error(`the board reply has no text "${key}"`);
  return t;
}

const checkNetwork = (n) => {
  if (n !== 'test' && n !== 'main') throw new Error(`network must be "test" or "main", not ${n}`);
};

export class Device {
  #link;
  #session = null;
  /** Set once the session cannot be trusted to line up any more. */
  #broken = null;

  constructor(link) {
    this.#link = link;
  }

  get paired() {
    return !!this.#session && !this.#broken;
  }

  get link() {
    return this.#link;
  }

  async #roundtrip(type, payload, timeout) {
    // No request ids in this protocol: a frame nobody asked for would be
    // taken as the answer to the next request.
    this.#link.drain();
    await this.#link.send(type, payload);
    return this.#link.recv(timeout);
  }

  async #plain(req, timeout = SHORT) {
    const { type, payload } = await this.#roundtrip(FRAME.REQUEST, encodeCbor(req), timeout);
    if (type !== FRAME.RESPONSE && type !== FRAME.ERROR) throw new Error(`unexpected frame type 0x${type.toString(16)} to a plaintext request`);
    return decodeReply(type, payload);
  }

  /** A plaintext request, resent while the board says it is busy with the user. */
  async #plainRetrying(req, { signal, onBusy } = {}) {
    const deadline = Date.now() + USER_TIMEOUT;
    for (;;) {
      try {
        return await this.#plain(req);
      } catch (e) {
        if (!(e instanceof DeviceError) || e.code !== ERR.BUSY || Date.now() > deadline || signal?.aborted) throw e;
        onBusy?.();
        await sleep(1000);
      }
    }
  }

  /**
   * The v2 handshake, then wait for the user to compare the six digits and
   * press ALLOW on the board. `onPasskey(digits)` shows them. Resolves once
   * the board answers an encrypted ping.
   */
  async pair({ onPasskey, onBusy, signal, timeoutMs = USER_TIMEOUT } = {}) {
    this.#session = null;
    this.#broken = null;
    const { privateKey, publicKey: hostPub } = await generateKeyPair();
    const ack = await this.#plainRetrying({ method: 'hello', version: PROTOCOL_VERSION, hostPubkey: hostPub }, { signal, onBusy });
    if (ack?.version !== PROTOCOL_VERSION) {
      throw new Error(`the board speaks protocol version ${ack?.version}, this wallet ${PROTOCOL_VERSION}: update the older side`);
    }
    const devicePub = bytesField(ack, 'devicePubkey', 32);
    const commit = bytesField(ack, 'deviceCommit', 32);

    const hostNonce = randomBytes(NONCE_BYTES);
    const r = await this.#plainRetrying({ method: 'helloReveal', hostNonce }, { signal, onBusy });
    const deviceNonce = bytesField(r, 'deviceNonce', NONCE_BYTES);
    // Not optional: an unverified nonce is one a relay could choose after
    // seeing everything else (PROTOCOL.md §3).
    const expected = await commitment(devicePub, hostPub, deviceNonce);
    if (toHex(expected) !== toHex(commit)) throw new Error("the board's commitment does not open: refusing to pair (possible relay)");

    const shared = await sharedSecret(privateKey, devicePub);
    const keys = await deriveKeys(shared, hostPub, devicePub, hostNonce, deviceNonce);
    const session = new Session(keys);
    onPasskey?.(keys.passkey);

    // The same sealed ping, resent until the user presses ALLOW: until then
    // the board answers in plaintext and moves no counter.
    const sealed = session.seal(encodeCbor({ method: 'ping' }));
    const deadline = Date.now() + timeoutMs;
    for (;;) {
      if (signal?.aborted) throw new Error('pairing cancelled');
      const { type, payload } = await this.#roundtrip(FRAME.ENC_REQUEST, sealed, SHORT);
      if (type === FRAME.ENC_RESPONSE) {
        decodeReply(FRAME.RESPONSE, session.open(payload));
        this.#session = session;
        return keys.passkey;
      }
      if (type !== FRAME.ERROR) throw new Error(`unexpected frame type 0x${type.toString(16)} while pairing`);
      if (Date.now() > deadline) throw new Error('the pairing was not confirmed on the board in time');
      await sleep(500);
    }
  }

  /** One encrypted request and its reply. */
  async call(method, fields = {}, timeout = SHORT) {
    try {
      return await this.#call(method, fields, timeout);
    } catch (e) {
      if (e instanceof DeviceError && e.code === ERR.BOARD_NETWORK && fields.network) {
        const out = new DeviceError(e.code, e.deviceMessage);
        out.message = wrongNetworkMessage(fields.network);
        throw out;
      }
      throw e;
    }
  }

  async #call(method, fields, timeout) {
    if (this.#broken) throw new Error(`${this.#broken}; reconnect to the board`);
    const session = this.#session;
    if (!session) throw new Error('not paired with the board');
    const sealed = session.seal(encodeCbor({ ...fields, method }));
    for (let attempt = 0; ; attempt++) {
      let frame;
      try {
        frame = await this.#roundtrip(FRAME.ENC_REQUEST, sealed, timeout);
      } catch (e) {
        // A late reply would pair with the next request and every counter
        // after it: this session is over.
        if (e instanceof LinkTimeout) this.#broken = `the board did not answer ${method}`;
        else this.#broken = e.message;
        throw e;
      }
      const { type, payload } = frame;
      if (type === FRAME.ENC_RESPONSE || type === FRAME.ENC_ERROR) {
        let opened;
        try {
          opened = session.open(payload);
        } catch (e) {
          this.#broken = e.message;
          throw e;
        }
        return decodeReply(type, opened);
      }
      if (type === FRAME.ERROR) {
        // Plaintext: the frame never decrypted, so no counter moved.
        try {
          return decodeReply(type, payload);
        } catch (e) {
          if (e instanceof DeviceError && e.code === ERR.BUSY && attempt < 30) {
            await sleep(300); // a full queue on BLE: resend the same frame
            continue;
          }
          if (e instanceof DeviceError && e.code === ERR.SESSION) this.#broken = 'the board dropped the session (it restarted, locked the link or paired with someone else)';
          throw e;
        }
      }
      this.#broken = `unexpected frame type 0x${type.toString(16)}`;
      throw new Error(this.#broken);
    }
  }

  /**
   * Ask the board to prompt for its PIN if it is locked, and wait until the
   * user has entered it there (the PIN never crosses the wire).
   */
  async ensureUnlocked({ onWaiting, signal, timeoutMs = USER_TIMEOUT } = {}) {
    const r = await this.call('unlock');
    if (r?.unlocked === 1) return;
    onWaiting?.();
    const deadline = Date.now() + timeoutMs;
    while (Date.now() < deadline) {
      if (signal?.aborted) throw new Error('cancelled while waiting for the PIN');
      await sleep(1000);
      const s = await this.call('getStatus');
      if (s?.unlocked === 1) return;
    }
    throw new Error('the board was not unlocked in time');
  }

  zGetFeatures() {
    return this.call('zGetFeatures', {});
  }

  /** UFVK, UA, t-address, ZIP 32 seed fingerprint and (if set) birthday. */
  async zGetAccount(account, network) {
    checkNetwork(network);
    const a = await this.call('zGetAccount', { account, network }, DERIVE);
    return {
      account: uintField(a, 'account'),
      network: textField(a, 'network'),
      seedFingerprint: toHex(bytesField(a, 'seedFingerprint', 32)),
      ufvk: textField(a, 'ufvk'),
      ua: textField(a, 'ua'),
      tAddress: textField(a, 'tAddress'),
      birthday: Number.isInteger(a.birthday) ? a.birthday : null,
    };
  }

  /** Record the birthday on the board (per network). True if kept in flash. */
  async zSetBirthday(account, network, birthday) {
    checkNetwork(network);
    const r = await this.call('zSetAccountMeta', { account, network, birthday }, DERIVE);
    return uintField(r, 'stored') === 1;
  }

  /**
   * The board shows the address (any of the account's diversified ones)
   * for the user to compare, and answers only after they say it matches.
   */
  async zShowAddress(account, network, ua) {
    checkNetwork(network);
    let r;
    try {
      r = await this.call('zShowAddress', { account, network, ua }, USER_TIMEOUT);
    } catch (e) {
      if (e instanceof DeviceError) {
        const why =
          e.code === (ERR.ZSIGN_BASE | 14)
            ? 'The board says this address is NOT from its wallet. Do not use it to receive: this wallet may be compromised.'
            : e.code === (ERR.ZSIGN_BASE | 16)
              ? 'This address has a receiver the board cannot check (for example Sapling), so it was not shown.'
              : e.code === ERR.USER_REJECTED
                ? 'You answered on the board that the addresses do not match.'
                : e.code === ERR.USER_TIMEOUT
                  ? 'No answer on the board in time.'
                  : null;
        if (why) {
          const out = new DeviceError(e.code, e.deviceMessage);
          out.message = why;
          throw out;
        }
      }
      throw e;
    }
    const shown = textField(r, 'ua');
    // Firmware without the rotated-address check ignores "ua" and shows its
    // default address: then this catches the difference.
    if (shown !== ua) throw new Error(`the board confirmed ${shown}, but this wallet shows ${ua}`);
    return { ua: shown, tAddress: r.tAddress ?? null };
  }

  /**
   * Upload a PCZT (the signer view), let the user review it on the board,
   * and return the sighash they approved with the board's signatures.
   */
  async zSign(pczt, account, network, onProgress) {
    checkNetwork(network);
    const sha = await sha256(pczt);
    const begin = await this.call('zSignBegin', { len: pczt.length, sha256: sha, account, network }, DERIVE);
    const max = uintField(begin, 'maxChunk');
    if (max === 0) throw new Error('the board offered a zero chunk size');
    let off = 0;
    while (off < pczt.length) {
      const n = Math.min(max, pczt.length - off);
      const r = await this.call('zSignChunk', { offset: off, data: pczt.slice(off, off + n) });
      off += n;
      if (uintField(r, 'received') !== off) throw new Error(`the board's received count disagrees after ${off} bytes`);
      onProgress?.({ phase: 'upload', sent: off, total: pczt.length });
    }
    onProgress?.({ phase: 'review' });
    const done = await this.call('zSignFinish', {}, USER_TIMEOUT);
    const sighash = bytesField(done, 'sighash', 32);
    if (!Array.isArray(done.signatures)) throw new Error('the board reply has no signatures');
    const entries = done.signatures.map((s) => {
      const pool = uintField(s, 'pool');
      const index = uintField(s, 'index');
      if (pool === POOL.TRANSPARENT) {
        const sig = bytesField(s, 'sig');
        if (sig.length < 9 || sig.length > 73) throw new Error(`transparent signature of ${sig.length} bytes`);
        return { pool, index, sig, pubkey: bytesField(s, 'pubkey', 33) };
      }
      if (pool !== POOL.ORCHARD && pool !== POOL.IRONWOOD) throw new Error(`the board returned a signature for pool ${pool}, which this wallet cannot apply`);
      return { pool, index, sig: bytesField(s, 'sig', 64) };
    });
    return { sighash, sha256: sha, entries };
  }
}
