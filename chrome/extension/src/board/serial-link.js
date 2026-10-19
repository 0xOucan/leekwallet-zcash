// The board over USB, through Web Serial, with the "LK" frames.
//
// Where it runs: the offscreen document. `navigator.serial` exists in
// documents (not in the service worker), and the offscreen document lives
// until the browser closes, so a signature in progress survives the popup
// closing. `requestPort()` needs a user gesture in a tab, though (the
// toolbar popup closes when the chooser takes focus), so a tab asks once,
// and the grant, kept against the extension's origin, lets the offscreen
// document's `getPorts()` find the board from then on.
//
// From LeekWallet's EVM extension (firmware/app/extension/src/serial-transport.ts, in git history),
// which learned on hardware: DTR/RTS are deasserted right after open (on the
// ESP32-S3's USB-Serial-JTAG they reset the chip), and the boot log the
// reset pours out is drained until the line goes quiet before the first
// request; `reader.cancel()` before `port.close()`, or the close hangs.
//
// The port is held only while a session is: every way a session ends
// (Disconnect, Switch to Bluetooth, a timeout, an error, a network switch,
// a Bluetooth connect, the last extension page gone) comes to `close()`,
// and `closeAll()` closes whatever this document still has open. A port
// left open here kept the board's USB busy while it served Bluetooth (seen
// in Brave 152: Bluetooth then got "derivation failed" from the board).
// No step of `close()` may hang it: a write the board never takes (its
// Link set to Bluetooth) would keep `writer.close()` waiting forever, so
// the writer is aborted, and each step is given a few seconds at most.

import { UsbFrameDecoder, encodeUsbFrame } from './framing.js';
import { FrameQueue, LinkTimeout, linkTimeoutHint } from './link.js';

/** Espressif's VID; the ESP32-S3 USB-Serial-JTAG PID. A chooser filter, not identity: the passkey is. */
export const USB_FILTERS = [{ usbVendorId: 0x303a, usbProductId: 0x1001 }, { usbVendorId: 0x303a }];

const SETTLE_QUIET_MS = 300;
const SETTLE_MIN_MS = 400;
const SETTLE_MAX_MS = 3000;
/** The longest one frame may take to leave (a whole PCZT chunk is a few kB). */
const WRITE_MS = 10000;
/** The longest any one step of `close()` may take. */
const CLOSE_STEP_MS = 2000;

/** Every SerialLink of this document whose port is open, or opening. */
const openLinks = new Set();

/** Resolves when `p` settles or after `ms`, whichever is first; never rejects. */
function bounded(p, ms = CLOSE_STEP_MS) {
  let timer;
  return Promise.race([Promise.resolve(p).catch(() => {}), new Promise((r) => (timer = setTimeout(r, ms)))]).finally(() => clearTimeout(timer));
}

export function isBoardPort(port) {
  return port.getInfo().usbVendorId === 0x303a;
}

export function portLabel(port) {
  const { usbVendorId: v, usbProductId: p } = port.getInfo();
  const hex = (n) => n.toString(16).padStart(4, '0');
  return v === undefined ? 'USB serial' : p === undefined ? `USB ${hex(v)}` : `USB ${hex(v)}:${hex(p)}`;
}

export class SerialLink {
  kind = 'usb';
  #port;
  #reader = null;
  #writer = null;
  #queue = new FrameQueue();
  #decoder = new UsbFrameDecoder();
  #onClose;
  #closing = null;
  #writeMs;

  constructor(port, onClose, { writeMs = WRITE_MS } = {}) {
    this.#port = port;
    this.#writeMs = writeMs;
    this.label = portLabel(port);
    this.#onClose = onClose;
  }

  static async open(port, onClose, opts) {
    const l = new SerialLink(port, onClose, opts);
    try {
      await l.#open();
    } catch (e) {
      // Whatever was opened goes: a half-open link must not hold the port.
      await l.close();
      throw e;
    }
    return l;
  }

  /** How many links this document holds open (0 when no session is). */
  static get openCount() {
    return openLinks.size;
  }

  /** Closes every link this document still has open. */
  static async closeAll() {
    await Promise.all([...openLinks].map((l) => l.close()));
  }

  async #open() {
    await this.#port.open({ baudRate: 115200 }); // CDC ignores the rate
    openLinks.add(this);
    this.#writer = this.#port.writable.getWriter();
    this.#reader = this.#port.readable.getReader();
    try {
      await this.#port.setSignals({ dataTerminalReady: false, requestToSend: false });
    } catch {
      // Not every backend has it; nothing more to do.
    }
    const { pending } = await this.#settle(this.#reader);
    void this.#pump(this.#reader, pending);
  }

  /**
   * Discards boot noise until the line has been quiet a moment. Returns
   * `{ pending }`, the read still waiting when it stops (a read cannot be
   * taken back): what it brings is the board's first answer, so the pump
   * takes it. Wrapped: an async function returning the promise itself
   * would wait for it.
   */
  async #settle(reader) {
    const start = Date.now();
    let last = start;
    let heard = false;
    let read = null;
    for (;;) {
      const now = Date.now();
      if (now - start >= SETTLE_MAX_MS) return { pending: read };
      if (now - start >= SETTLE_MIN_MS && (!heard || now - last >= SETTLE_QUIET_MS)) return { pending: read };
      const wait = Math.max(10, Math.min(SETTLE_MAX_MS - (now - start), heard ? SETTLE_QUIET_MS - (now - last) : SETTLE_MIN_MS - (now - start)));
      const timedOut = Symbol('t');
      let timer;
      read ??= reader.read();
      const r = await Promise.race([read, new Promise((res) => (timer = setTimeout(() => res(timedOut), wait)))]).catch(() => null);
      clearTimeout(timer);
      if (r === timedOut) continue;
      read = null;
      if (!r || r.done) return { pending: null };
      heard = true;
      last = Date.now();
    }
  }

  async #pump(reader, pending) {
    try {
      for (;;) {
        const { value, done } = await (pending ?? reader.read());
        pending = null;
        if (done) break;
        if (value?.length) for (const f of this.#decoder.push(value)) this.#queue.push(f);
      }
      this.#fail('the board closed the connection');
    } catch (e) {
      this.#fail(e?.message ?? 'the serial link failed');
    }
  }

  #fail(why) {
    if (this.#queue.closed) return;
    this.#queue.close(why);
    this.#onClose?.(why);
  }

  async send(type, payload) {
    if (this.#queue.closed) throw new Error(`the serial port is closed: ${this.#queue.closed}`);
    // A board serving Bluetooth may never take the bytes: the write would
    // wait forever, and the session with it. Past WRITE_MS the link fails.
    let timer;
    const stuck = new Promise((_, reject) => {
      timer = setTimeout(() => reject(linkTimeoutHint(new LinkTimeout(this.#writeMs), 'USB')), this.#writeMs);
    });
    try {
      await Promise.race([this.#writer.write(encodeUsbFrame(type, payload)), stuck]);
    } catch (e) {
      if (e instanceof LinkTimeout) this.#fail('the board did not take what was sent over USB');
      throw e;
    } finally {
      clearTimeout(timer);
    }
  }

  recv(timeoutMs) {
    return this.#queue.next(timeoutMs).catch((e) => {
      throw linkTimeoutHint(e, 'USB');
    });
  }

  drain() {
    this.#queue.drain();
  }

  /** Releases the reader and writer and closes the port. Safe to call twice. */
  close() {
    this.#closing ??= this.#close();
    return this.#closing;
  }

  async #close() {
    this.#queue.close('closed');
    await bounded(this.#reader?.cancel());
    try {
      this.#reader?.releaseLock();
    } catch {}
    // abort, not close: close() waits for queued bytes the board may never take.
    await bounded(this.#writer?.abort());
    try {
      this.#writer?.releaseLock();
    } catch {}
    if (openLinks.has(this)) await bounded(this.#port.close());
    openLinks.delete(this);
  }
}
