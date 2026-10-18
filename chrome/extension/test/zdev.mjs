// zdev (firmware/sim) for the extension's tests: the firmware's own
// protocol, session, signers and review pages on a pseudo-terminal, with
// the abandon-art seed. Builds it with make (incremental) and starts it;
// links to it as the board would be linked over USB (the "LK" frames) or,
// through a fake GATT server, over Bluetooth (chunks at MTU 23).
//
// Development only: zdev never holds a seed with real funds.
import { spawn, spawnSync } from 'node:child_process';
import { constants, mkdtempSync, openSync, existsSync, rmSync, writeSync, closeSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import tty from 'node:tty';
import { fileURLToPath } from 'node:url';

import { BleReassembler, FRAME, UsbFrameDecoder, chunkForBle, encodeFrame, encodeUsbFrame } from '../src/board/framing.js';
import { FrameQueue } from '../src/board/link.js';

const here = dirname(fileURLToPath(import.meta.url));
export const repo = join(here, '..', '..', '..');
const sim = join(repo, 'firmware/sim');

let built = null;
/** make -C firmware/sim build/zdev; null if it cannot be built. */
export function zdevBinary() {
  if (built !== null) return built || null;
  const r = spawnSync('make', ['-C', sim, 'build/zdev'], { stdio: process.env.VERBOSE ? 'inherit' : 'pipe', env: { CARGO_BUILD_JOBS: '2', ...process.env } });
  built = r.status === 0 ? join(sim, 'build/zdev') : false;
  if (!built) console.error(`zdev did not build:\n${r.stderr?.toString().slice(-2000)}`);
  return built || null;
}

/**
 * A running zdev. `yes`: answer every question on the "device" yes; else
 * write answers with `answer('y')`. `passphrase`: a BIP 39 passphrase on
 * the abandon-art mnemonic, for a board with another wallet. `passkeys`
 * collects what it showed.
 */
export async function startZdev({ yes = true, passphrase = null } = {}) {
  const bin = zdevBinary();
  if (!bin) return null;
  const dir = mkdtempSync(join(tmpdir(), 'leek-zdev-'));
  const link = join(dir, 'port');
  // A passphrase makes another seed, so another wallet (another board).
  const args = ['--link', link, ...(yes ? ['--yes'] : []), ...(passphrase ? ['--passphrase', passphrase] : [])];
  const child = spawn(bin, args, { stdio: ['pipe', 'pipe', 'pipe'] });
  const z = { child, link, dir, passkeys: [], out: '', questions: 0 };
  child.stdout.on('data', (d) => {
    const s = d.toString();
    z.out += s;
    if (process.env.VERBOSE) process.stdout.write(`  [zdev] ${s}`);
    for (const m of s.matchAll(/Passkey on this device: (\d{6})/g)) z.passkeys.push(m[1]);
  });
  child.stderr.on('data', (d) => (z.out += d.toString()));
  z.answer = (a) => child.stdin.write(`${a}\n`);
  z.stop = () => {
    child.kill();
    rmSync(dir, { recursive: true, force: true });
  };
  const t0 = Date.now();
  while (!existsSync(link)) {
    if (Date.now() - t0 > 10e3) throw new Error(`zdev did not start: ${z.out}`);
    await new Promise((r) => setTimeout(r, 50));
  }
  return z;
}

/** The serial side of the PTY, as SerialLink is the serial side of the board. */
export class PtyLink {
  kind = 'usb';
  label = 'zdev';
  #fd;
  #stream;
  #queue = new FrameQueue();
  #decoder = new UsbFrameDecoder();

  constructor(path) {
    this.#fd = openSync(path, constants.O_RDWR | constants.O_NOCTTY);
    this.#stream = new tty.ReadStream(this.#fd);
    this.#stream.on('data', (b) => {
      for (const f of this.#decoder.push(new Uint8Array(b))) this.#queue.push(f);
    });
    this.#stream.on('error', (e) => this.#queue.close(e.message));
  }

  /** Raw bytes, for tests that interleave console noise. */
  writeRaw(bytes) {
    writeSync(this.#fd, bytes);
  }

  async send(type, payload) {
    writeSync(this.#fd, encodeUsbFrame(type, payload));
  }

  recv(ms) {
    return this.#queue.next(ms);
  }

  drain() {
    this.#queue.drain();
  }

  async close() {
    this.#queue.close('closed');
    this.#stream.destroy();
    try {
      closeSync(this.#fd);
    } catch {}
  }
}

/**
 * A stand-in for a Web Bluetooth BluetoothDevice whose GATT server is zdev:
 * it reassembles the host's chunks into frames, sends them to the PTY in
 * "LK" frames (what zdev speaks), and chunks zdev's replies back as
 * notifications at MTU 23, exactly as the board's ble.c does.
 */
export function fakeBluetoothDevice(path, { mtu = 23 } = {}) {
  const pty = new PtyLink(path);
  const reasm = new BleReassembler();
  const listeners = { gattserverdisconnected: [] };
  const rx = {
    listeners: [],
    addEventListener: (ev, f) => ev === 'characteristicvaluechanged' && rx.listeners.push(f),
    startNotifications: async () => {
      (async () => {
        for (;;) {
          let f;
          try {
            f = await pty.recv(3600e3);
          } catch {
            return;
          }
          for (const c of chunkForBle(encodeFrame(f.type, f.payload), mtu)) {
            const value = new DataView(c.buffer);
            for (const l of rx.listeners) l({ target: { value } });
          }
        }
      })();
      return rx;
    },
    stopNotifications: async () => rx,
  };
  const stats = { writes: 0, maxWrite: 0 };
  const tx = {
    writeValueWithResponse: async (chunk) => {
      stats.writes++;
      stats.maxWrite = Math.max(stats.maxWrite, chunk.length);
      if (chunk.length > mtu - 3) throw new Error(`a ${chunk.length}-byte write does not fit MTU ${mtu}`);
      const f = reasm.push(chunk);
      if (f) await pty.send(f.type, f.payload);
    },
  };
  const service = { getCharacteristic: async (uuid) => (uuid.endsWith('2') ? tx : rx) };
  return {
    name: 'LeekWallet (zdev)',
    stats,
    addEventListener: (ev, f) => listeners[ev]?.push(f),
    gatt: {
      connect: async () => ({ getPrimaryService: async () => service }),
      disconnect: () => {
        void pty.close();
        for (const f of listeners.gattserverdisconnected) f();
      },
    },
  };
}

export { FRAME };
