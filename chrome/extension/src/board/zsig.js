// The `.zsig` signatures-only file (docs/zcash/FORMATS.md), as
// host/leek-device/src/zsig.rs writes and reads it:
//
//   0   4   "ZSIG"
//   4   1   version: 1 if every entry is shielded, 2 if any is transparent
//   5   32  SHA-256 of the exact PCZT bytes the device checked and signed
//   37  2   count N (u16 LE)
//   39  ..  entries: pool (1) | index (u32 LE) | then
//             pool 1, 2: signature (64, RedPallas R || S)
//             pool 0:    L (1) | DER signature || sighash type (L) | pubkey (33)
//
// The engine (`applyZsig`, `finish`) parses it with zsig.rs itself; this
// side only writes what the board returned.

export const POOL = { TRANSPARENT: 0, ORCHARD: 1, IRONWOOD: 2 };
const TSIG_MIN = 9;
const TSIG_MAX = 73;

/**
 * entries: [{ pool, index, sig: Uint8Array, pubkey?: Uint8Array }] as the
 * board's zSignFinish answers them; pcztSha256: 32 bytes.
 */
export function encodeZsig(pcztSha256, entries) {
  if (pcztSha256.length !== 32) throw new Error('the PCZT hash is not 32 bytes');
  if (entries.length > 0xffff) throw new Error('too many signatures for a .zsig');
  const anyT = entries.some((e) => e.pool === POOL.TRANSPARENT);
  const out = [0x5a, 0x53, 0x49, 0x47, anyT ? 2 : 1, ...pcztSha256, entries.length & 0xff, entries.length >> 8];
  for (const e of entries) {
    if (!Number.isInteger(e.index) || e.index < 0 || e.index > 0xffffffff) throw new Error(`bad signature index ${e.index}`);
    out.push(e.pool, e.index & 0xff, (e.index >>> 8) & 0xff, (e.index >>> 16) & 0xff, e.index >>> 24);
    if (e.pool === POOL.TRANSPARENT) {
      if (e.sig.length < TSIG_MIN || e.sig.length > TSIG_MAX) throw new Error(`transparent signature of ${e.sig.length} bytes`);
      if (!e.pubkey || e.pubkey.length !== 33) throw new Error('transparent signature without its 33-byte key');
      out.push(e.sig.length, ...e.sig, ...e.pubkey);
    } else if (e.pool === POOL.ORCHARD || e.pool === POOL.IRONWOOD) {
      if (e.sig.length !== 64) throw new Error(`shielded signature of ${e.sig.length} bytes`);
      out.push(...e.sig);
    } else {
      throw new Error(`the board returned a signature for pool ${e.pool}, which this wallet cannot apply`);
    }
  }
  return Uint8Array.from(out);
}

/** The reverse, for tests and for checking a file before handing it over. */
export function parseZsig(bytes) {
  const fail = (why) => {
    throw new Error(`.zsig: ${why}`);
  };
  if (bytes.length < 39) fail('truncated');
  if (String.fromCharCode(...bytes.slice(0, 4)) !== 'ZSIG') fail('bad magic');
  const version = bytes[4];
  if (version !== 1 && version !== 2) fail(`version ${version}`);
  const pcztSha256 = bytes.slice(5, 37);
  const n = bytes[37] | (bytes[38] << 8);
  let at = 39;
  const take = (k) => {
    if (at + k > bytes.length) fail('truncated');
    const s = bytes.slice(at, at + k);
    at += k;
    return s;
  };
  const entries = [];
  for (let i = 0; i < n; i++) {
    const h = take(5);
    const pool = h[0];
    const index = (h[1] | (h[2] << 8) | (h[3] << 16) | (h[4] << 24)) >>> 0;
    if (pool === POOL.TRANSPARENT) {
      if (version === 1) fail('a transparent entry in a version 1 file');
      const l = take(1)[0];
      if (l < TSIG_MIN || l > TSIG_MAX) fail(`transparent signature of ${l} bytes`);
      entries.push({ pool, index, sig: take(l), pubkey: take(33) });
    } else if (pool === POOL.ORCHARD || pool === POOL.IRONWOOD) {
      entries.push({ pool, index, sig: take(64) });
    } else {
      fail(`unknown pool ${pool}`);
    }
  }
  if (at !== bytes.length) fail(`${bytes.length - at} trailing bytes`);
  return { version, pcztSha256, entries };
}
