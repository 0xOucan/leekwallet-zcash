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
// From LeekWallet's extension (firmware/app/extension/src/serial-transport.ts),
// which learned on hardware: DTR/RTS are deasserted right after open (on the
// ESP32-S3's USB-Serial-JTAG they reset the chip), and the boot log the
// reset pours out is drained until the line goes quiet before the first
// request; `reader.cancel()` before `port.close()`, or the close hangs.

import { UsbFrameDecoder, encodeUsbFrame } from './framing.js';
import { FrameQueue, linkTimeoutHint } from './link.js';

/** Espressif's VID; the ESP32-S3 USB-Serial-JTAG PID. A chooser filter, not identity: the passkey is. */
export const USB_FILTERS = [{ usbVendorId: 0x303a, usbProductId: 0x1001 }, { usbVendorId: 0x303a }];

const SETTLE_QUIET_MS = 300;
const SETTLE_MIN_MS = 400;
const SETTLE_MAX_MS = 3000;

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

  constructor(port, onClose) {
    this.#port = port;
    this.label = portLabel(port);
    this.#onClose = onClose;
  }

  static async open(port, onClose) {
    const l = new SerialLink(port, onClose);
    await l.#open();
    return l;
  }

  async #open() {
    await this.#port.open({ baudRate: 115200 }); // CDC ignores the rate
    this.#writer = this.#port.writable.getWriter();
    this.#reader = this.#port.readable.getReader();
    try {
      await this.#port.setSignals({ dataTerminalReady: false, requestToSend: false });
    } catch {
      // Not every backend has it; nothing more to do.
    }
    await this.#settle(this.#reader);
    void this.#pump(this.#reader);
  }

  /** Discards boot noise until the line has been quiet a moment. */
  async #settle(reader) {
    const start = Date.now();
    let last = start;
    let heard = false;
    for (;;) {
      const now = Date.now();
      if (now - start >= SETTLE_MAX_MS) return;
      if (now - start >= SETTLE_MIN_MS && (!heard || now - last >= SETTLE_QUIET_MS)) return;
      const wait = Math.max(10, Math.min(SETTLE_MAX_MS - (now - start), heard ? SETTLE_QUIET_MS - (now - last) : SETTLE_MIN_MS - (now - start)));
      const timedOut = Symbol('t');
      let timer;
      const r = await Promise.race([reader.read(), new Promise((res) => (timer = setTimeout(() => res(timedOut), wait)))]).catch(() => null);
      clearTimeout(timer);
      if (r === timedOut) continue;
      if (!r || r.done) return;
      heard = true;
      last = Date.now();
    }
  }

  async #pump(reader) {
    try {
      for (;;) {
        const { value, done } = await reader.read();
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
    await this.#writer.write(encodeUsbFrame(type, payload));
  }

  recv(timeoutMs) {
    return this.#queue.next(timeoutMs).catch((e) => {
      throw linkTimeoutHint(e, 'USB');
    });
  }

  drain() {
    this.#queue.drain();
  }

  async close() {
    this.#queue.close('closed');
    try {
      await this.#reader?.cancel();
    } catch {}
    try {
      this.#reader?.releaseLock();
    } catch {}
    try {
      await this.#writer?.close();
    } catch {}
    try {
      this.#writer?.releaseLock();
    } catch {}
    try {
      await this.#port.close();
    } catch {}
  }
}
