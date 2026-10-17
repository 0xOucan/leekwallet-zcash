// The board over Bluetooth LE, through Web Bluetooth.
//
// GATT layout (firmware/docs/PROTOCOL.md §2): one service, a write
// characteristic host -> board and a notify characteristic board -> host.
// Frames carry no "LK" marker and travel in chunks with a 1-byte header.
// Web Bluetooth does not expose the negotiated ATT MTU, so writes assume the
// 23-byte floor (19 payload bytes per chunk), which is always safe: a chunk
// larger than the link allows is truncated by the stack, silently.
//
// Where it runs: in the extension tab that chose the device. A
// BluetoothDevice cannot be handed to another document, `requestDevice()`
// needs a user gesture in a tab (the toolbar popup closes when the chooser
// takes focus), and `navigator.bluetooth.getDevices()`, which would let the
// offscreen document find it again, is still behind a flag in Chrome. So a
// Bluetooth board is driven from that tab, and the tab stays open while it
// signs.

import { BleReassembler, chunkForBle, encodeFrame } from './framing.js';
import { FrameQueue } from './link.js';

export const GATT = {
  service: '6c65656b-7761-6c6c-6574-000000000001',
  hostToDevice: '6c65656b-7761-6c6c-6574-000000000002',
  deviceToHost: '6c65656b-7761-6c6c-6574-000000000003',
};

/** The ATT MTU writes are cut for (the floor; see above). */
export const BLE_MTU = 23;

export class BleLink {
  kind = 'ble';
  #device;
  #tx = null;
  #rx = null;
  #queue = new FrameQueue();
  #reasm = new BleReassembler();
  #onClose;
  #writing = Promise.resolve();

  constructor(device, onClose) {
    this.#device = device;
    this.label = device.name ? `Bluetooth ${device.name}` : 'Bluetooth';
    this.#onClose = onClose;
  }

  /** Must be called from a click: Chrome shows its device chooser. */
  static request() {
    if (!globalThis.navigator?.bluetooth) throw new Error('This browser has no Web Bluetooth.');
    return navigator.bluetooth.requestDevice({ filters: [{ services: [GATT.service] }] });
  }

  static async open(device, onClose) {
    const l = new BleLink(device, onClose);
    await l.#open();
    return l;
  }

  async #open() {
    this.#device.addEventListener('gattserverdisconnected', () => this.#fail('the board disconnected'));
    const server = await this.#device.gatt.connect();
    const service = await server.getPrimaryService(GATT.service);
    this.#tx = await service.getCharacteristic(GATT.hostToDevice);
    this.#rx = await service.getCharacteristic(GATT.deviceToHost);
    this.#rx.addEventListener('characteristicvaluechanged', (ev) => this.#onNotify(ev.target.value));
    await this.#rx.startNotifications();
  }

  #onNotify(view) {
    const chunk = new Uint8Array(view.buffer, view.byteOffset, view.byteLength).slice();
    try {
      const f = this.#reasm.push(chunk);
      if (f) this.#queue.push(f);
    } catch (e) {
      // A broken chunk sequence loses that frame; the request it answered
      // times out and the session is dropped, as on USB.
      console.warn('BLE reassembly:', e.message);
    }
  }

  #fail(why) {
    if (this.#queue.closed) return;
    this.#queue.close(why);
    this.#onClose?.(why);
  }

  async send(type, payload) {
    if (this.#queue.closed) throw new Error(`the Bluetooth link is closed: ${this.#queue.closed}`);
    const chunks = chunkForBle(encodeFrame(type, payload), BLE_MTU);
    // Write with response: the stack paces the chunks, none is dropped.
    const p = this.#writing.then(async () => {
      for (const c of chunks) await this.#tx.writeValueWithResponse(c);
    });
    this.#writing = p.catch(() => {});
    await p;
  }

  recv(timeoutMs) {
    return this.#queue.next(timeoutMs);
  }

  drain() {
    this.#queue.drain();
    this.#reasm.reset();
  }

  async close() {
    this.#queue.close('closed');
    try {
      await this.#rx?.stopNotifications();
    } catch {}
    // Disconnect explicitly: the board serves one peer at a time.
    try {
      this.#device.gatt.disconnect();
    } catch {}
  }
}
