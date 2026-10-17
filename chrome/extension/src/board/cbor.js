// Minimal CBOR (RFC 8949): unsigned and negative integers, byte strings,
// text strings, arrays and maps with text keys. What the board's protocol
// uses, nothing more.
//
// A JavaScript copy of LeekWallet's firmware/app/packages/core/src/cbor.ts
// (types stripped, same grammar, same refusals): canonical encoding with
// shortest-form heads and sorted map keys; tags, floats, indefinite lengths
// and 64-bit arguments are refused rather than tolerated.

const UINT = 0;
const NEGINT = 1;
const BYTES = 2;
const TEXT = 3;
const ARRAY = 4;
const MAP = 5;

function head(out, major, value) {
  if (!Number.isInteger(value) || value < 0) throw new Error(`CBOR head needs a non-negative integer, got ${value}`);
  const m = major << 5;
  if (value < 24) out.push(m | value);
  else if (value < 0x100) out.push(m | 24, value);
  else if (value < 0x10000) out.push(m | 25, value >> 8, value & 0xff);
  else if (value <= 0xffffffff) out.push(m | 26, value >>> 24, (value >>> 16) & 0xff, (value >>> 8) & 0xff, value & 0xff);
  else throw new Error(`${value} exceeds the 32-bit CBOR limit`);
}

const utf8 = new TextEncoder();

function encodeInto(out, v) {
  if (typeof v === 'number') {
    if (!Number.isInteger(v)) throw new Error(`floats are not supported: ${v}`);
    if (v >= 0) head(out, UINT, v);
    else head(out, NEGINT, -v - 1);
  } else if (typeof v === 'string') {
    const b = utf8.encode(v);
    head(out, TEXT, b.length);
    for (const x of b) out.push(x);
  } else if (v instanceof Uint8Array) {
    head(out, BYTES, v.length);
    for (const x of v) out.push(x);
  } else if (Array.isArray(v)) {
    head(out, ARRAY, v.length);
    for (const x of v) encodeInto(out, x);
  } else if (v !== null && typeof v === 'object') {
    const keys = Object.keys(v).filter((k) => v[k] !== undefined).sort();
    head(out, MAP, keys.length);
    for (const k of keys) {
      encodeInto(out, k);
      encodeInto(out, v[k]);
    }
  } else {
    throw new Error(`cannot encode ${typeof v} as CBOR`);
  }
}

export function encodeCbor(value) {
  const out = [];
  encodeInto(out, value);
  return Uint8Array.from(out);
}

class Reader {
  constructor(buf) {
    this.buf = buf;
    this.pos = 0;
  }
  byte() {
    const b = this.buf[this.pos];
    if (b === undefined) throw new Error('truncated CBOR');
    this.pos++;
    return b;
  }
  take(n) {
    if (n < 0 || this.pos + n > this.buf.length) throw new Error('truncated CBOR');
    const out = this.buf.slice(this.pos, this.pos + n);
    this.pos += n;
    return out;
  }
  arg(info) {
    if (info < 24) return info;
    if (info === 24) return this.byte();
    if (info === 25) return (this.byte() << 8) | this.byte();
    if (info === 26) return this.byte() * 0x1000000 + (this.byte() << 16) + (this.byte() << 8) + this.byte();
    if (info === 27) throw new Error('64-bit CBOR arguments are not supported');
    if (info === 31) throw new Error('indefinite-length CBOR is not supported');
    throw new Error(`reserved CBOR additional info ${info}`);
  }
}

const textDecoder = new TextDecoder('utf-8', { fatal: true });

function decodeFrom(r, depth) {
  if (depth > 8) throw new Error('CBOR nesting too deep');
  const initial = r.byte();
  const major = initial >> 5;
  const n = r.arg(initial & 0x1f);
  switch (major) {
    case UINT:
      return n;
    case NEGINT:
      return -n - 1;
    case BYTES:
      return r.take(n);
    case TEXT:
      return textDecoder.decode(r.take(n));
    case ARRAY: {
      const out = [];
      for (let i = 0; i < n; i++) out.push(decodeFrom(r, depth + 1));
      return out;
    }
    case MAP: {
      const out = {};
      for (let i = 0; i < n; i++) {
        const k = decodeFrom(r, depth + 1);
        if (typeof k !== 'string') throw new Error('CBOR map keys must be text');
        out[k] = decodeFrom(r, depth + 1);
      }
      return out;
    }
    default:
      throw new Error(`unsupported CBOR major type ${major}`);
  }
}

export function decodeCbor(bytes) {
  const r = new Reader(bytes);
  const v = decodeFrom(r, 0);
  if (r.pos !== bytes.length) throw new Error(`${bytes.length - r.pos} trailing bytes after CBOR`);
  return v;
}
