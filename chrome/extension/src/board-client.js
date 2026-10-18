// The board, from a page (the popup, or the popup opened as a tab).
//
// Two kinds, one interface (`run(op, params)`, `state`, `onState(f)`):
//
// - RemoteBoard: the USB board held by the offscreen document. Calls go
//   over the engine port as `board.<op>`; a signature keeps going if the
//   popup closes, and the popup finds the result when it opens again.
// - LocalBoard: a Bluetooth board held by this tab (a BluetoothDevice
//   cannot leave the page that chose it). The tab must stay open while it
//   signs; the engine is still the offscreen one, through EngineClient.
//
// Choosing a device needs a click in a tab (Chrome's chooser closes the
// toolbar popup): `grantUsb()` and `LocalBoard.chooseBluetooth()` must be
// called straight from a click handler, before any other await.

import { BleLink } from './board/ble-link.js';
import { BoardHost } from './board/host.js';
import { USB_FILTERS } from './board/serial-link.js';

export class RemoteBoard {
  kind = 'usb';
  #engine;
  #listeners = new Set();

  constructor(engine) {
    this.#engine = engine;
    this.state = engine.board ?? { phase: 'disconnected' };
    engine.onBoard((s) => {
      this.state = s;
      for (const f of this.#listeners) f(s);
    });
  }

  run(op, params = {}) {
    return this.#engine.call(`board.${op}`, params);
  }

  onState(f) {
    this.#listeners.add(f);
  }
}

export class LocalBoard {
  kind = 'ble';
  #host;
  #listeners = new Set();
  #chosen = null;

  constructor(engine) {
    this.#host = new BoardHost({
      engine: (m, p) => engine.call(m, p),
      openLink: async (kind, onClose) => {
        if (kind !== 'ble' || !this.#chosen) throw new Error('choose the Bluetooth board first');
        return BleLink.open(this.#chosen, onClose);
      },
      onState: (s) => {
        this.state = s;
        for (const f of this.#listeners) f(s);
      },
    });
    this.state = this.#host.state;
  }

  /** From a click: Chrome's Bluetooth chooser, filtered to the board's service. */
  async chooseBluetooth() {
    this.#chosen = await BleLink.request();
    return this.#chosen.name ?? 'Bluetooth device';
  }

  run(op, params = {}) {
    return this.#host.run(op, params);
  }

  onState(f) {
    this.#listeners.add(f);
  }
}

/** From a click in a tab: the browser's USB serial chooser. True if a port was chosen. */
export async function grantUsb() {
  if (!navigator.serial) throw new Error('This browser has no Web Serial: use desktop Chrome, Edge or Brave.');
  try {
    await navigator.serial.requestPort({ filters: USB_FILTERS });
    return true;
  } catch (e) {
    if (e?.name === 'NotFoundError') return false; // closed the chooser, or it was empty
    throw e;
  }
}

/**
 * Why there is no `navigator.bluetooth`, and how to turn it on. Brave hides
 * Web Bluetooth behind its own flag; on Linux, Chromium (Chrome and Brave)
 * also keeps it behind "Experimental Web Platform features" (Web Bluetooth
 * CG, implementation-status.md).
 */
export function bluetoothHelp(nav = navigator) {
  const ua = nav.userAgent ?? '';
  const linux = /Linux/.test(ua) && !/Android|CrOS/.test(ua);
  if (nav.brave?.isBrave) {
    return `Web Bluetooth is off in Brave. Enable brave://flags/#brave-web-bluetooth-api${linux ? ' and, on Linux, also brave://flags/#enable-experimental-web-platform-features' : ''}, relaunch Brave, then press Bluetooth again.`;
  }
  if (linux) return 'On Linux, Chrome has Web Bluetooth only with chrome://flags/#enable-experimental-web-platform-features enabled. Enable it, relaunch, then press Bluetooth again.';
  return 'This browser has no Web Bluetooth. Use desktop Chrome, Edge or Brave, or connect over USB.';
}

export const inTab = () => new URLSearchParams(location.search).has('tab');
