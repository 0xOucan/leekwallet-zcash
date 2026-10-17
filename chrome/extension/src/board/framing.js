// The board's two wire formats (firmware/docs/PROTOCOL.md §2):
//
//   USB (Web Serial)   'L' 'K' | len:u16 | type | payload
//   BLE (GATT)         chunk header | (len:u16 | type | payload, split)
//
// `len` is big-endian and counts the type byte and the payload. On USB the
// port also carries the ESP-IDF console, so frames start with the "LK" sync
// marker and the decoder skips log text between them. A GATT
// characteristic carries only frames, so BLE has no marker, but writes are
// capped at MTU - 3, so each frame is cut into chunks with a 1-byte header:
// bit 7 "more follows", bits 0-6 a sequence number wrapping at 128.
//
// Ported from LeekWallet's extension (firmware/app/extension/src/usb-framing.ts)
// and core (firmware/app/packages/core/src/framing.ts), which the firmware's
// own tests match.

export const FRAME = {
  REQUEST: 0x01,
  RESPONSE: 0x02,
  ENC_REQUEST: 0x11,
  ENC_RESPONSE: 0x12,
  ENC_ERROR: 0x7e,
  ERROR: 0x7f,
};

/** PROTOCOL_MAX_FRAME in firmware/src/protocol.h. */
export const MAX_FRAME = 4096;

const SYNC0 = 0x4c; // 'L'
const SYNC1 = 0x4b; // 'K'

function concat(a, b) {
  const out = new Uint8Array(a.length + b.length);
  out.set(a, 0);
  out.set(b, a.length);
  return out;
}

/** len | type | payload: the frame without a marker (BLE, and the USB body). */
export function encodeFrame(type, payload) {
  const body = payload.length + 1;
  if (body + 2 > MAX_FRAME) throw new Error(`a frame of ${body + 2} bytes is over the ${MAX_FRAME}-byte limit`);
  const out = new Uint8Array(body + 2);
  out[0] = body >> 8;
  out[1] = body & 0xff;
  out[2] = type;
  out.set(payload, 3);
  return out;
}

/** 'L' 'K' | len | type | payload, for the serial port. */
export function encodeUsbFrame(type, payload) {
  return concat(Uint8Array.of(SYNC0, SYNC1), encodeFrame(type, payload));
}

/**
 * Finds frames in a USB byte stream that also carries console text. Feed
 * whatever was read; it returns the frames completed so far.
 */
export class UsbFrameDecoder {
  #buf = new Uint8Array(0);

  push(bytes) {
    if (bytes.length) {
      this.#buf = concat(this.#buf, bytes);
      // Unparsed console text is normal, but not without bound.
      if (this.#buf.length > 4 * MAX_FRAME) this.#buf = this.#buf.slice(this.#buf.length - 4 * MAX_FRAME);
    }
    const frames = [];
    for (let f = this.#next(); f; f = this.#next()) frames.push(f);
    return frames;
  }

  reset() {
    this.#buf = new Uint8Array(0);
  }

  #next() {
    for (;;) {
      const b = this.#buf;
      let start = -1;
      for (let i = 0; i + 1 < b.length; i++) {
        if (b[i] === SYNC0 && b[i + 1] === SYNC1) {
          start = i;
          break;
        }
      }
      if (start < 0) {
        // Keep the last byte: it may be the 'L' of a marker split by a read.
        this.#buf = b.slice(Math.max(0, b.length - 1));
        return null;
      }
      if (start > 0) this.#buf = b.slice(start);
      if (this.#buf.length < 4) return null;
      const body = (this.#buf[2] << 8) | this.#buf[3];
      if (body < 1 || body + 4 > MAX_FRAME) {
        // Not a length the device sends: this "LK" was log text. Rescan.
        this.#buf = this.#buf.slice(2);
        continue;
      }
      if (this.#buf.length < body + 4) return null;
      const frame = { type: this.#buf[4], payload: this.#buf.slice(5, body + 4) };
      this.#buf = this.#buf.slice(body + 4);
      return frame;
    }
  }
}

/** Splits a BLE frame (no marker) into GATT writes for an ATT MTU. */
export function chunkForBle(frame, mtu = 23) {
  const capacity = mtu - 3 - 1; // ATT header, then our chunk header
  if (capacity < 1) throw new Error(`MTU ${mtu} is too small`);
  const chunks = [];
  for (let off = 0, seq = 0; off < frame.length; off += capacity, seq++) {
    const slice = frame.slice(off, off + capacity);
    const more = off + capacity < frame.length;
    const c = new Uint8Array(slice.length + 1);
    c[0] = (more ? 0x80 : 0) | (seq & 0x7f);
    c.set(slice, 1);
    chunks.push(c);
  }
  return chunks;
}

/**
 * Reassembles BLE notifications into frames. A chunk out of sequence,
 * empty, or past the frame limit resets it and throws, as the firmware's
 * ble_chunk_push does: the peer is not authenticated yet when this runs.
 */
export class BleReassembler {
  #parts = [];
  #seq = 0;
  #total = 0;

  /** Returns a decoded frame { type, payload } when a chunk completes one. */
  push(chunk) {
    if (!chunk.length) throw new Error('empty BLE chunk');
    const seq = chunk[0] & 0x7f;
    const more = (chunk[0] & 0x80) !== 0;
    if (seq !== this.#seq) {
      const want = this.#seq;
      this.reset();
      throw new Error(`BLE chunk out of order: expected ${want}, got ${seq}`);
    }
    if (chunk.length < 2) {
      this.reset();
      throw new Error('BLE chunk carries no payload');
    }
    if (this.#total + chunk.length - 1 > MAX_FRAME) {
      this.reset();
      throw new Error(`BLE frame over ${MAX_FRAME} bytes`);
    }
    this.#parts.push(chunk.slice(1));
    this.#total += chunk.length - 1;
    this.#seq = (this.#seq + 1) & 0x7f;
    if (more) return null;
    const frame = new Uint8Array(this.#total);
    let off = 0;
    for (const p of this.#parts) {
      frame.set(p, off);
      off += p.length;
    }
    this.reset();
    return decodeFrame(frame);
  }

  reset() {
    this.#parts = [];
    this.#seq = 0;
    this.#total = 0;
  }
}

/** len | type | payload -> { type, payload }; the length must agree. */
export function decodeFrame(frame) {
  if (frame.length < 3) throw new Error('frame too short');
  const body = (frame[0] << 8) | frame[1];
  if (body < 1 || body + 2 !== frame.length) throw new Error(`frame length ${body} does not match ${frame.length - 2} bytes`);
  return { type: frame[2], payload: frame.slice(3) };
}
