// Import from the board's QR: the board's account without a cable, the
// desktop app's flow (app/src/qr-accounts.ts, src-tauri/src/qr_accounts.rs).
//
//   1. On the board: Main menu → View key, confirmed there. It shows its
//      account as an animated ur:leek-zcash-accounts: the account record of
//      host/leek-zcash/src/record.rs (seed fingerprint, UFVK, account
//      index; a label and a birthday only when a companion set them).
//   2. The webcam (src/qr/scan.js) feeds every code to leek-ur's decoder
//      (src/qr/ur.js); the record is read here and checked: the network of
//      the viewing key against the wallet chosen in the header.
//   3. With a birthday (typed, the record's, or the chain tip minus
//      BIRTHDAY_MARGIN, as the companion does), the engine's importAccount
//      makes it a device account: the engine parses the UFVK and derives
//      the account's addresses from it, so nothing but the viewing key
//      is taken on trust. Signable over QR, USB or Bluetooth.
//
// One way only: a viewing key comes in, nothing goes to the board. This is
// never pairing; USB and Bluetooth keep their 6-digit pairing code.
//
// No DOM here: tested under Node (test/qr.test.mjs).

export const ACCOUNT_RECORD_UR_TYPE = 'leek-zcash-accounts';
/** The companion's margin below the tip for a board with no birthday (app/src/zcash-auto.ts). */
export const BIRTHDAY_MARGIN = 288;
/** ZIP 32 account indices are below 2^31. */
const MAX_INDEX = 0x7fffffff;

// ------------------------------------------------- the record's CBOR

/**
 * The subset the record uses (RFC 8949): unsigned integers, byte and text
 * strings, arrays, maps with integer keys, and tags (skipped, as record.rs
 * accepts tagged account maps). Definite lengths only. Maps come back as
 * arrays of [key, value]; byte strings as Uint8Array.
 */
function readCbor(bytes) {
  let pos = 0;
  const byte = () => {
    if (pos >= bytes.length) throw new Error('the record ends early');
    return bytes[pos++];
  };
  const arg = (info) => {
    if (info < 24) return info;
    if (info === 24) return byte();
    if (info === 25) return (byte() << 8) | byte();
    if (info === 26) return byte() * 0x1000000 + (byte() << 16) + (byte() << 8) + byte();
    if (info === 27) {
      let n = 0n;
      for (let i = 0; i < 8; i++) n = (n << 8n) | BigInt(byte());
      return n;
    }
    throw new Error('indefinite or reserved CBOR lengths are not read');
  };
  const take = (n) => {
    if (typeof n !== 'number' || pos + n > bytes.length) throw new Error('the record ends early');
    const out = bytes.slice(pos, pos + n);
    pos += n;
    return out;
  };
  const text = new TextDecoder('utf-8', { fatal: true });
  const item = (depth) => {
    if (depth > 8) throw new Error('the record nests too deep');
    const b = byte();
    const major = b >> 5;
    const n = arg(b & 0x1f);
    switch (major) {
      case 0:
        return n;
      case 1:
        return { negative: n };
      case 2:
        return take(n);
      case 3:
        return text.decode(take(n));
      case 4: {
        const out = [];
        for (let i = 0; i < n; i++) out.push(item(depth + 1));
        return out;
      }
      case 5: {
        const out = [];
        for (let i = 0; i < n; i++) out.push([item(depth + 1), item(depth + 1)]);
        return { map: out };
      }
      case 6:
        return item(depth + 1); // a tag: its content
      default:
        throw new Error(`CBOR major type ${major} is not part of the record`);
    }
  };
  const v = item(0);
  if (pos !== bytes.length) throw new Error(`${bytes.length - pos} bytes after the record`);
  return v;
}

const get = (m, key) => m.map.find(([k]) => k === key)?.[1];

function u32(v, what) {
  if (v === undefined) return null;
  if (typeof v !== 'number' || !Number.isInteger(v) || v < 0 || v > 0xffffffff) throw new Error(`${what} is not a number`);
  return v;
}

/** The UFVK's network from its Bech32m prefix ("main" or "test"), or null. */
export function ufvkNetwork(ufvk) {
  if (/^uview1[02-9ac-hj-np-z]+$/.test(ufvk)) return 'main';
  if (/^uviewtest1[02-9ac-hj-np-z]+$/.test(ufvk)) return 'test';
  return null;
}

/** record.rs AccountRecord::decode: `{ seedFingerprint (hex), accounts: [{ ufvk, index, name, birthday }] }`. */
export function decodeAccountRecord(bytes) {
  const v = readCbor(bytes);
  if (!v?.map) throw new Error('not an account record');
  const fp = get(v, 1);
  if (!(fp instanceof Uint8Array) || fp.length !== 32) throw new Error('the record has no seed fingerprint');
  const items = get(v, 2);
  if (!Array.isArray(items)) throw new Error('the record has no accounts');
  const accounts = items.map((it) => {
    if (!it?.map) throw new Error('an account of the record is not a map');
    const ufvk = get(it, 1);
    if (typeof ufvk !== 'string' || !ufvkNetwork(ufvk)) throw new Error('an account of the record has no unified full viewing key');
    const index = u32(get(it, 2), 'the account index');
    if (index === null || index > MAX_INDEX) throw new Error('the account index is out of range');
    const name = get(it, 3);
    if (name !== undefined && typeof name !== 'string') throw new Error('the account label is not text');
    return { ufvk, index, name: name ?? null, birthday: u32(get(it, 4), 'the birthday') };
  });
  const seedFingerprint = Array.from(fp, (b) => b.toString(16).padStart(2, '0')).join('');
  return { seedFingerprint, accounts };
}

const LABEL = { main: 'Mainnet', test: 'Testnet' };

/**
 * qr_accounts.rs check_record: the record as `network`'s wallet would
 * import it, or why it cannot. The engine's import then parses the UFVK
 * for real (the board account's shape: Orchard + P2PKH).
 */
export function checkRecord(network, bytes) {
  let r;
  try {
    r = decodeAccountRecord(bytes);
  } catch (e) {
    throw new Error(`not a readable account record: ${e.message}`);
  }
  if (!r.accounts.length) throw new Error('the account record holds no account');
  for (const a of r.accounts) {
    const theirs = ufvkNetwork(a.ufvk);
    if (theirs !== network) {
      throw new Error(`this is a ${LABEL[theirs]} viewing key, and the ${LABEL[network]} wallet is chosen here: pick ${LABEL[theirs]} and scan again`);
    }
  }
  return { network, seedFingerprint: r.seedFingerprint, zfp: r.seedFingerprint.slice(0, 8).toUpperCase(), accounts: r.accounts };
}

/** A typed height, the record's, or the tip minus BIRTHDAY_MARGIN. */
export async function resolveBirthday(typed, recorded, tip) {
  const t = String(typed ?? '').trim();
  if (t) {
    const n = Number(t);
    if (!/^\d+$/.test(t) || !Number.isSafeInteger(n) || n < 1) {
      throw new Error('Enter the birthday as a block height (digits only), or leave it empty for a new seed.');
    }
    return n;
  }
  if (recorded !== null && recorded !== undefined) return recorded;
  return Math.max(1, (await tip()).height - BIRTHDAY_MARGIN);
}

// ------------------------------------------------- the import

/**
 * Phases: idle, scanning, found (the record is read: confirm and import),
 * importing, done, failed.
 */
export class QrAccountImport {
  phase = 'idle';
  received = 0;
  total = 0;
  note = '';
  error = null;
  record = null;
  network = 'test';
  onChange = () => {};
  #ur;
  #scan = null;

  constructor(ur) {
    this.#ur = ur;
  }

  start(network) {
    this.network = network;
    this.received = 0;
    this.total = 0;
    this.note = '';
    this.error = null;
    this.record = null;
    this.#scan?.free?.();
    this.#scan = new this.#ur.UrScan(ACCOUNT_RECORD_UR_TYPE);
    this.phase = 'scanning';
    this.onChange();
  }

  /** One decoded QR text. True once the scan should stop (found, or refused). */
  feed(text) {
    if (this.phase !== 'scanning' || !this.#scan) return false;
    const state = this.#scan.feed(String(text));
    if (state === 'ignored') return false;
    this.received = this.#scan.received;
    this.total = this.#scan.total;
    if (state === 'rejected') this.note = this.#scan.error ?? 'a QR code that is not the board’s view key';
    else if (state === 'accepted') this.note = '';
    if (state === 'complete') {
      try {
        this.record = checkRecord(this.network, this.#scan.takeMessage());
      } catch (e) {
        this.fail(e);
        return true;
      }
      this.phase = 'found';
      this.onChange();
      return true;
    }
    this.onChange();
    return false;
  }

  /** The account to import (the board sends one). */
  account() {
    return this.record?.accounts[0] ?? null;
  }

  /**
   * The engine's importAccount as a device account, born at `typed`, the
   * record's birthday, or the tip minus BIRTHDAY_MARGIN. `engine(method,
   * params)` is the engine call.
   */
  async importFound(engine, typed = '') {
    const r = this.record;
    const a = this.account();
    if (this.phase !== 'found' || !r || !a) throw new Error('Scan the board’s view key first.');
    const birthday = await resolveBirthday(typed, a.birthday, () => engine('tip', {}));
    this.phase = 'importing';
    this.error = null;
    this.onChange();
    try {
      const out = await engine('importAccount', {
        ufvk: a.ufvk,
        birthday,
        name: a.name ?? `LeekWallet #${a.index}`,
        origin: { seedFingerprint: r.seedFingerprint, accountIndex: a.index },
      });
      this.phase = 'done';
      this.onChange();
      return { ...out, birthday };
    } catch (e) {
      // Still found: fix the birthday and import again.
      this.phase = 'found';
      this.error = String(e?.message ?? e);
      this.onChange();
      throw e;
    }
  }

  cancel() {
    this.phase = 'idle';
    this.record = null;
    this.onChange();
  }

  fail(e) {
    this.phase = 'failed';
    this.error = String(e?.message ?? e);
    this.onChange();
  }
}

/** The scan's progress line. */
export function accountsCaption(q) {
  const base =
    q.total === 0
      ? 'Point the webcam at the board’s View key QR (Main menu → View key)…'
      : `${q.received} of ${q.total} part${q.total === 1 ? '' : 's'} read…`;
  return q.note ? `${base} (ignored one code: ${q.note})` : base;
}
