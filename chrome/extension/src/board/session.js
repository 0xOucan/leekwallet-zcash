// The v2 session's cryptography (firmware/docs/PROTOCOL.md §3), host side:
// X25519, a device commitment opened after the host reveals its nonce, HKDF-
// SHA-256 over the whole transcript into two directional keys and a six-
// digit passkey, then ChaCha20-Poly1305 with counter nonces.
//
// Same derivation as host/leek-device/src/device.rs and LeekWallet's
// packages/core/src/session.ts, with the same labels. X25519, SHA-256 and
// HKDF come from WebCrypto (the browser's own implementation; X25519 since
// Chrome 133); ChaCha20-Poly1305, which WebCrypto lacks, from @noble/ciphers
// (vendored by build.mjs from the pinned npm package, as LeekWallet's
// extension bundles it).

import { chacha20poly1305 } from '../vendor/noble-ciphers/chacha.js';

export const PROTOCOL_VERSION = 2;
export const NONCE_BYTES = 16;

const LABEL = {
  h2d: 'leek-session-h2d-v2',
  d2h: 'leek-session-d2h-v2',
  passkey: 'leek-session-passkey-v2',
  commit: 'leek-session-commit-v2',
  transcript: 'leek-session-transcript-v2',
};

const subtle = globalThis.crypto.subtle;
const enc = new TextEncoder();

export function concat(...parts) {
  const out = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
  let at = 0;
  for (const p of parts) {
    out.set(p, at);
    at += p.length;
  }
  return out;
}

export async function sha256(bytes) {
  return new Uint8Array(await subtle.digest('SHA-256', bytes));
}

export function randomBytes(n) {
  return globalThis.crypto.getRandomValues(new Uint8Array(n));
}

/** An ephemeral X25519 key pair: { privateKey: CryptoKey, publicKey: 32 bytes }. */
export async function generateKeyPair() {
  const kp = await subtle.generateKey({ name: 'X25519' }, false, ['deriveBits']);
  const publicKey = new Uint8Array(await subtle.exportKey('raw', kp.publicKey));
  return { privateKey: kp.privateKey, publicKey };
}

/** X25519(a, B). Refuses an all-zero result (a small-order peer key). */
export async function sharedSecret(privateKey, peerPublic) {
  if (peerPublic.length !== 32) throw new Error('the device public key is not 32 bytes');
  const peer = await subtle.importKey('raw', peerPublic, { name: 'X25519' }, false, []);
  let bits;
  try {
    bits = new Uint8Array(await subtle.deriveBits({ name: 'X25519', public: peer }, privateKey, 256));
  } catch (e) {
    throw new Error(`X25519 failed (a small-order device key?): ${e?.message ?? e}`);
  }
  if (bits.every((b) => b === 0)) throw new Error('degenerate shared secret: the device key is small-order');
  return bits;
}

/** H(label ‖ PKb ‖ PKa ‖ Nb): the device's commitment to its nonce. */
export function commitment(devicePub, hostPub, deviceNonce) {
  return sha256(concat(enc.encode(LABEL.commit), devicePub, hostPub, deviceNonce));
}

async function hkdf(ikm, salt, label) {
  const key = await subtle.importKey('raw', ikm, 'HKDF', false, ['deriveBits']);
  return new Uint8Array(await subtle.deriveBits({ name: 'HKDF', hash: 'SHA-256', salt, info: enc.encode(label) }, key, 256));
}

/**
 * Keys and passkey from the X25519 secret and the transcript, by role (host
 * public key first, then the device's, then the host nonce, then the device's).
 */
export async function deriveKeys(shared, hostPub, devicePub, hostNonce, deviceNonce) {
  if (hostPub.length !== 32 || devicePub.length !== 32 || hostNonce.length !== NONCE_BYTES || deviceNonce.length !== NONCE_BYTES) {
    throw new Error('transcript fields are the wrong length');
  }
  const salt = await sha256(concat(enc.encode(LABEL.transcript), hostPub, devicePub, hostNonce, deviceNonce));
  const p = await hkdf(shared, salt, LABEL.passkey);
  const n = ((p[0] << 24) >>> 0) + (p[1] << 16) + (p[2] << 8) + p[3];
  return {
    h2d: await hkdf(shared, salt, LABEL.h2d),
    d2h: await hkdf(shared, salt, LABEL.d2h),
    passkey: String(n % 1_000_000).padStart(6, '0'),
  };
}

/** 4 zero bytes ‖ 8-byte big-endian counter, as the firmware builds it. */
function nonceFor(counter) {
  const n = new Uint8Array(12);
  new DataView(n.buffer).setBigUint64(4, BigInt(counter));
  return n;
}

function sameBytes(a, b) {
  if (a.length !== b.length) return false;
  for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false;
  return true;
}

/**
 * An established session, host side. The send counter moves only when a
 * reply opens: the device answers a frame it could not decrypt (before the
 * user pressed ALLOW) in plaintext and moves no counter, so the host resends
 * the identical sealed frame. Sealing a different message under a held
 * counter would reuse a nonce, which this refuses.
 */
export class Session {
  #send;
  #recv;
  #tx = 0;
  #rx = 0;
  #held = null;

  constructor(keys) {
    this.#send = keys.h2d;
    this.#recv = keys.d2h;
    this.passkey = keys.passkey;
  }

  seal(plain) {
    if (this.#held && !sameBytes(this.#held, plain)) {
      throw new Error('refusing to seal a second message under one nonce: the previous request was never answered');
    }
    this.#held = plain.slice();
    return chacha20poly1305(this.#send, nonceFor(this.#tx)).encrypt(plain);
  }

  /** Throws if the frame does not authenticate: the channel is then unusable. */
  open(ct) {
    let out;
    try {
      out = chacha20poly1305(this.#recv, nonceFor(this.#rx)).decrypt(ct);
    } catch {
      throw new Error('a device reply failed authentication; the channel is not trustworthy');
    }
    this.#rx++;
    this.#tx++;
    this.#held = null;
    return out;
  }
}
