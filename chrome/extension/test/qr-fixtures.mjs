// Shared by test/qr.test.mjs and test/e2e.mjs: the leek-ur WASM module
// (chrome/ur/pkg) loaded in Node, and the board's View key record as
// host/leek-zcash/src/record.rs writes it.
import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));

/** chrome/ur/pkg, initialised in Node (chrome/ur/build.sh first). */
export async function loadUrNode() {
  const pkg = join(here, '..', '..', 'ur', 'pkg');
  const ur = await import(join(pkg, 'leek_chrome_ur.js'));
  ur.initSync({ module: readFileSync(join(pkg, 'leek_chrome_ur_bg.wasm')) });
  return ur;
}

// ------------------------------------------------ CBOR for the record

function cborHead(major, n) {
  if (n < 24) return [(major << 5) | n];
  if (n < 0x100) return [(major << 5) | 24, n];
  if (n < 0x10000) return [(major << 5) | 25, n >> 8, n & 0xff];
  return [(major << 5) | 26, n >>> 24, (n >>> 16) & 0xff, (n >>> 8) & 0xff, n & 0xff];
}
export const enc = {
  uint: (n) => cborHead(0, n),
  bytes: (b) => [...cborHead(2, b.length), ...b],
  text: (s) => {
    const b = new TextEncoder().encode(s);
    return [...cborHead(3, b.length), ...b];
  },
  array: (items) => [...cborHead(4, items.length), ...items.flat()],
  map: (pairs) => [...cborHead(5, pairs.length), ...pairs.flat(2)],
  tag: (t, v) => [...cborHead(6, t), ...v],
};

/** record.rs AccountRecord::encode, for the board's one account. */
export function record({ fp, ufvk, index = 0, name = null, birthday = null, tagged = false }) {
  const acct = [[enc.uint(1), enc.text(ufvk)], [enc.uint(2), enc.uint(index)]];
  if (name !== null) acct.push([enc.uint(3), enc.text(name)]);
  if (birthday !== null) acct.push([enc.uint(4), enc.uint(birthday)]);
  const a = enc.map(acct);
  return new Uint8Array(enc.map([[enc.uint(1), enc.bytes(fp)], [enc.uint(2), enc.array([tagged ? enc.tag(49203, a) : a])]]));
}

