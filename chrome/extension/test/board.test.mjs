// The board protocol module (src/board/) in Node, no browser, no board:
// framing, CBOR, .zsig, and the whole session and every Zcash call against
// zdev, the firmware's protocol on a PTY (firmware/sim). Over the USB
// framing directly, and over Bluetooth through a fake GATT server that
// chunks at MTU 23 as the board does.
//
//   cd chrome/extension && npm install && node --test test/board.test.mjs
//
// zdev is built with make (needs the firmware sim's toolchain: cc, cargo).
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { after, before, describe, test } from 'node:test';

import { vendor } from '../vendor.mjs';
await vendor();

const { decodeCbor, encodeCbor } = await import('../src/board/cbor.js');
const { BleReassembler, FRAME, UsbFrameDecoder, chunkForBle, encodeFrame, encodeUsbFrame } = await import('../src/board/framing.js');
const { Device, DeviceError, ERR, toHex, fromHex: fromHexT, wrongNetworkMessage } = await import('../src/board/device.js');
const { BoardHost, BoardNeeds } = await import('../src/board/host.js');
const { BleLink } = await import('../src/board/ble-link.js');
const { SerialLink } = await import('../src/board/serial-link.js');
const { LinkTimeout, linkTimeoutHint } = await import('../src/board/link.js');
const { bluetoothHelp } = await import('../src/board-client.js');
const { encodeZsig, parseZsig, POOL } = await import('../src/board/zsig.js');
const { Session, deriveKeys, generateKeyPair, sharedSecret } = await import('../src/board/session.js');
const { repo, startZdev, PtyLink, fakeBluetoothDevice, zdevBinary } = await import('./zdev.mjs');

const keys = JSON.parse(readFileSync(join(repo, 'vectors/keys.json'), 'utf8'));
const key = (name) => keys.find((k) => k.name === name);
const pczts = JSON.parse(readFileSync(join(repo, 'vectors/pczt.json'), 'utf8'));
function vector(name) {
  const v = pczts.find((e) => e.name === name);
  return { v, bytes: new Uint8Array(readFileSync(join(repo, 'vectors', v.file))) };
}
const sha256hex = (b) => createHash('sha256').update(b).digest('hex');

describe('framing', () => {
  test('USB frames are found among console text, split anywhere', () => {
    const f1 = encodeUsbFrame(FRAME.RESPONSE, Uint8Array.of(1, 2, 3));
    const f2 = encodeUsbFrame(FRAME.ENC_RESPONSE, new Uint8Array(300).fill(7));
    const noise = new TextEncoder().encode('I (1234) LK boot: hello\r\n');
    const stream = Uint8Array.from([...noise, ...f1, ...noise, ...f2]);
    for (const cut of [1, 2, 5, 31, stream.length - 1]) {
      const d = new UsbFrameDecoder();
      const got = [...d.push(stream.slice(0, cut)), ...d.push(stream.slice(cut))];
      assert.equal(got.length, 2, `cut at ${cut}`);
      assert.deepEqual([...got[0].payload], [1, 2, 3]);
      assert.equal(got[1].type, FRAME.ENC_RESPONSE);
      assert.equal(got[1].payload.length, 300);
    }
  });

  test('BLE chunks at MTU 23 carry 19 bytes and reassemble, a Permit2-sized frame included', () => {
    const frame = encodeFrame(FRAME.ENC_REQUEST, new Uint8Array(1500).map((_, i) => i));
    const chunks = chunkForBle(frame, 23);
    assert.ok(chunks.every((c) => c.length <= 20));
    assert.equal(chunks.length, Math.ceil(frame.length / 19));
    assert.equal(chunks.at(-1)[0] & 0x80, 0);
    const r = new BleReassembler();
    let out = null;
    for (const c of chunks) out = r.push(c) ?? out;
    assert.equal(out.type, FRAME.ENC_REQUEST);
    assert.equal(out.payload.length, 1500);
  });

  test('BLE reassembly refuses a chunk out of sequence or empty', () => {
    const chunks = chunkForBle(encodeFrame(FRAME.REQUEST, new Uint8Array(60)), 23);
    const r = new BleReassembler();
    r.push(chunks[0]);
    assert.throws(() => r.push(chunks[2]), /out of order/);
    assert.throws(() => r.push(Uint8Array.of(0x80)), /no payload/);
  });
});

describe('cbor and .zsig', () => {
  test('CBOR round trip, canonical key order', () => {
    const v = { method: 'zSignChunk', offset: 70000, data: Uint8Array.of(1, 2), list: [-1, 'x'] };
    const b = encodeCbor(v);
    assert.deepEqual(decodeCbor(b), v);
    assert.deepEqual(encodeCbor({ b: 1, a: 2 }), encodeCbor({ a: 2, b: 1 }));
    assert.throws(() => decodeCbor(Uint8Array.of(0xf9, 0, 0)), /major type 7/);
  });

  test('the .zsig vector parses and re-encodes byte for byte', () => {
    const file = new Uint8Array(readFileSync(join(repo, 'vectors/zsig/iw_send_shielded.zsig')));
    const z = parseZsig(file);
    const { bytes } = vector('iw_send_shielded');
    assert.equal(toHex(z.pcztSha256), sha256hex(bytes));
    assert.equal(z.version, 1);
    assert.deepEqual(encodeZsig(z.pcztSha256, z.entries), file);
  });

  test('a transparent entry makes a version 2 .zsig', () => {
    const e = [{ pool: POOL.TRANSPARENT, index: 0, sig: new Uint8Array(71).fill(1), pubkey: new Uint8Array(33).fill(2) }];
    const b = encodeZsig(new Uint8Array(32), e);
    assert.equal(b[4], 2);
    assert.deepEqual(parseZsig(b).entries[0].pubkey, e[0].pubkey);
  });
});

describe('session crypto', () => {
  test('both ends derive the same keys; the passkey depends on every nonce', async () => {
    const a = await generateKeyPair();
    const b = await generateKeyPair();
    const s1 = await sharedSecret(a.privateKey, b.publicKey);
    const s2 = await sharedSecret(b.privateKey, a.publicKey);
    assert.deepEqual(s1, s2);
    const n = (x) => new Uint8Array(16).fill(x);
    const k = await deriveKeys(s1, a.publicKey, b.publicKey, n(1), n(2));
    assert.match(k.passkey, /^\d{6}$/);
    assert.notDeepEqual(k.h2d, k.d2h);
    const k2 = await deriveKeys(s1, a.publicKey, b.publicKey, n(1), n(3));
    assert.notDeepEqual(k2.h2d, k.h2d);
    // A device-side session (keys swapped) opens what the host seals.
    const host = new Session(k);
    const dev = new Session({ h2d: k.d2h, d2h: k.h2d });
    const ct = host.seal(Uint8Array.of(9, 9));
    assert.deepEqual(dev.open(ct), Uint8Array.of(9, 9));
    assert.throws(() => host.seal(Uint8Array.of(1)), /second message under one nonce/);
  });
});

const zdevOk = !!zdevBinary();

describe('against zdev over USB framing', { skip: !zdevOk && 'zdev could not be built' }, () => {
  let z;
  let dev;
  before(async () => {
    z = await startZdev({ yes: true });
    dev = new Device(new PtyLink(z.link));
  });
  after(async () => {
    await dev?.link.close();
    z?.stop();
  });

  test('pairs: the passkey shown is the one zdev shows', async () => {
    let shown = null;
    await dev.pair({ onPasskey: (p) => (shown = p) });
    await dev.ensureUnlocked();
    await new Promise((r) => setTimeout(r, 50));
    assert.match(shown, /^\d{6}$/);
    assert.equal(z.passkeys.at(-1), shown);
  });

  test('zGetFeatures', async () => {
    const f = await dev.zGetFeatures();
    assert.equal(f.showAddress, 1);
    assert.ok(f.spendPools.includes('transparent'));
  });

  test('zGetAccount: the testnet and mainnet accounts of the vectors', async () => {
    for (const network of ['test', 'main']) {
      await z.switchNetwork(network);
      const k = key(`${network}/abandon-art/pp0/acct0`);
      const a = await dev.zGetAccount(0, network);
      assert.equal(a.ufvk, k.ufvk, network);
      assert.equal(a.ua, k.ua, network);
      assert.equal(a.tAddress, k.transparent_address, network);
      assert.equal(a.seedFingerprint, k.seed_fingerprint, network);
    }
    await z.switchNetwork('test');
    const a1 = await dev.zGetAccount(1, 'test');
    assert.equal(a1.ufvk, key('test/abandon-art/pp0/acct1').ufvk);
  });

  test('zSetAccountMeta keeps one birthday per network', async () => {
    assert.equal((await dev.zGetAccount(0, 'test')).birthday, null);
    assert.equal(await dev.zSetBirthday(0, 'test', 4_501_000), true);
    await z.switchNetwork('main');
    assert.equal(await dev.zSetBirthday(0, 'main', 3_520_600), true);
    assert.equal((await dev.zGetAccount(0, 'main')).birthday, 3_520_600);
    await z.switchNetwork('test');
    assert.equal((await dev.zGetAccount(0, 'test')).birthday, 4_501_000);
  });

  test('the board answers only for its own network, and the message says which to change', async () => {
    const isWrongNetwork = (asked) => (e) => e instanceof DeviceError && e.code === ERR.BOARD_NETWORK && e.message === wrongNetworkMessage(asked);
    assert.match(wrongNetworkMessage('main'), /^The board is set to Testnet, but this wallet is on Mainnet\. Switch the board to Mainnet in Settings → Network/);
    await assert.rejects(dev.zGetAccount(0, 'main'), isWrongNetwork('main'));
    await assert.rejects(dev.zShowAddress(0, 'main', key('main/abandon-art/pp0/acct0').ua), isWrongNetwork('main'));
    await assert.rejects(dev.zSetBirthday(0, 'main', 3_600_000), isWrongNetwork('main'));
    await assert.rejects(dev.zSign(vector('main_iw_send_shielded_compact').bytes, 0, 'main'), isWrongNetwork('main'));
    assert.match(z.out, /Wrong network: the app asked for Mainnet; this device is set to Testnet/);
    await z.switchNetwork('main');
    await assert.rejects(dev.zGetAccount(0, 'test'), isWrongNetwork('test'));
    assert.match(wrongNetworkMessage('test'), /^The board is set to Mainnet, but this wallet is on Testnet\./);
    assert.equal((await dev.zGetAccount(0, 'main')).birthday, 3_520_600, 'a refused birthday replaced the stored one');
    await z.switchNetwork('test');
    assert.equal((await dev.zGetAccount(0, 'test')).ua, key('test/abandon-art/pp0/acct0').ua);
  });

  test('zShowAddress confirms the account address and refuses another', async () => {
    const k = key('test/abandon-art/pp0/acct0');
    const r = await dev.zShowAddress(0, 'test', k.ua);
    assert.equal(r.ua, k.ua);
    await assert.rejects(dev.zShowAddress(0, 'test', key('test/abandon-art/pp0/acct1').ua), (e) => e.code === (ERR.ZSIGN_BASE | 14) && /NOT from its wallet/.test(e.message));
  });

  test('zSign: a compact send; the sighash is the vector, the .zsig parses', async () => {
    const { v, bytes } = vector('iw_send_shielded_compact');
    const progress = [];
    const r = await dev.zSign(bytes, 0, 'test', (p) => progress.push(p.phase));
    assert.equal(toHex(r.sighash), v.sighash);
    assert.equal(toHex(r.sha256), sha256hex(bytes));
    assert.ok(progress.includes('upload') && progress.at(-1) === 'review');
    const zsig = parseZsig(encodeZsig(r.sha256, r.entries));
    assert.equal(zsig.version, 1);
    assert.equal(zsig.entries.length, 1);
    assert.equal(zsig.entries[0].pool, POOL.IRONWOOD);
  });

  test('zSign: a full (non-compact) PCZT in several chunks', async () => {
    const { v, bytes } = vector('iw_send_transparent');
    const r = await dev.zSign(bytes, 0, 'test');
    assert.equal(toHex(r.sighash), v.sighash);
  });

  test('zSign: a shield signs the transparent inputs (.zsig version 2)', async () => {
    const { v, bytes } = vector('t_shield_compact');
    const r = await dev.zSign(bytes, 0, 'test');
    assert.equal(toHex(r.sighash), v.sighash);
    const t = r.entries.filter((e) => e.pool === POOL.TRANSPARENT);
    assert.equal(t.length, v.t_inputs?.length ?? t.length);
    assert.ok(t.length >= 1);
    assert.equal(encodeZsig(r.sha256, r.entries)[4], 2);
  });

  test('zSign on mainnet', async () => {
    const { v, bytes } = vector('main_iw_send_shielded_compact');
    await z.switchNetwork('main');
    const r = await dev.zSign(bytes, 0, 'main');
    await z.switchNetwork('test');
    assert.equal(toHex(r.sighash), v.sighash);
  });

  test('zSign refuses a tampered PCZT and a network mismatch, and the session goes on', async () => {
    await assert.rejects(dev.zSign(vector('r_output_value_tampered').bytes, 0, 'test'), (e) => e instanceof DeviceError && e.code === (ERR.ZSIGN_BASE | 13));
    await assert.rejects(dev.zSign(vector('main_iw_send_shielded_compact').bytes, 0, 'test'), (e) => e instanceof DeviceError && (e.code & 0xff00) === ERR.ZSIGN_BASE);
    assert.equal((await dev.zGetAccount(0, 'test')).ua, key('test/abandon-art/pp0/acct0').ua);
  });

  test('a second pairing replaces the session', async () => {
    await dev.pair();
    assert.equal((await dev.zGetAccount(0, 'test')).ua, key('test/abandon-art/pp0/acct0').ua);
  });
});

describe('against zdev, answering on the "device"', { skip: !zdevOk && 'zdev could not be built' }, () => {
  test('a rejection on the board returns no signature; the session survives it', async () => {
    const z = await startZdev({ yes: false });
    const dev = new Device(new PtyLink(z.link));
    try {
      z.answer('y'); // pairing
      await dev.pair();
      z.answer('n'); // the review
      await assert.rejects(dev.zSign(vector('iw_send_shielded_compact').bytes, 0, 'test'), (e) => e.code === ERR.USER_REJECTED);
      z.answer('n'); // the address check
      await assert.rejects(dev.zShowAddress(0, 'test', key('test/abandon-art/pp0/acct0').ua), (e) => e.code === ERR.USER_REJECTED && /do not match/.test(e.message));
      z.answer('y');
      const { v, bytes } = vector('iw_send_shielded_compact');
      assert.equal(toHex((await dev.zSign(bytes, 0, 'test')).sighash), v.sighash);
    } finally {
      await dev.link.close();
      z.stop();
    }
  });

  test('pairing refused on the board never completes', async () => {
    const z = await startZdev({ yes: false });
    const dev = new Device(new PtyLink(z.link));
    try {
      z.answer('n');
      await assert.rejects(dev.pair({ timeoutMs: 3000 }), /not confirmed/);
      assert.equal(dev.paired, false);
    } finally {
      await dev.link.close();
      z.stop();
    }
  });
});

describe('BoardHost (the wallet flow) against zdev, with a stand-in engine', { skip: !zdevOk && 'zdev could not be built' }, () => {
  /** Records the engine calls the host makes and answers them as the engine would. */
  function fakeEngine(network) {
    const calls = [];
    const engine = async (method, params) => {
      calls.push({ method, params });
      if (method === 'status') return { open: true, network };
      if (method === 'tip') return { height: 4_600_000 };
      if (method === 'importAccount') return { imported: true, account: { uuid: 'u1', kind: 'device', origin: params.origin }, addresses: { unified: 'x' } };
      if (method === 'finish') return { txid: 'ab'.repeat(32), broadcast: params.broadcast, size: 9000, proveMs: 1 };
      throw new Error(`unexpected engine call ${method}`);
    };
    return { engine, calls };
  }

  test('connect, import (asking for the birthday, then recording it), verify, sign and finish', async () => {
    const z = await startZdev({ yes: true });
    const { engine, calls } = fakeEngine('test');
    const states = [];
    const host = new BoardHost({ engine, openLink: async () => new PtyLink(z.link), onState: (s) => states.push(s) });
    try {
      await host.run('connect', { kind: 'usb', network: 'test' });
      assert.equal(host.state.phase, 'ready');
      assert.ok(states.some((s) => s.phase === 'pairing' && /^\d{6}$/.test(s.passkey) && s.passkey === z.passkeys.at(-1)));
      // Which wallet: the popup shows a device account only to its board.
      assert.equal(host.state.seedFingerprint, key('test/abandon-art/pp0/acct0').seed_fingerprint);
      assert.ok(states.some((s) => s.phase === 'unlocking' && s.waiting === 'wallet'));

      await assert.rejects(host.run('importAccount', { network: 'test' }), (e) => e instanceof BoardNeeds && e.need === 'birthday' && e.tip === 4_600_000);
      const k = key('test/abandon-art/pp0/acct0');
      const imp = await host.run('importAccount', { network: 'test', birthday: 4_501_000 });
      const ic = calls.find((c) => c.method === 'importAccount').params;
      assert.equal(ic.ufvk, k.ufvk);
      assert.deepEqual(ic.origin, { seedFingerprint: k.seed_fingerprint, accountIndex: 0 });
      assert.equal(ic.birthday, 4_501_000);
      assert.equal(imp.storedOnBoard, true);
      // The next import takes the birthday from the board.
      const again = await host.run('importAccount', { network: 'test' });
      assert.equal(again.birthdayFromBoard, true);
      assert.equal(again.birthday, 4_501_000);

      assert.equal((await host.run('showAddress', { network: 'test', ua: k.ua })).ua, k.ua);

      const { bytes } = vector('t_shield_compact');
      const r = await host.run('signAndFinish', { network: 'test', signerView: toHex(bytes), broadcast: true });
      const fin = calls.find((c) => c.method === 'finish').params;
      assert.equal(fin.signerView, toHex(bytes));
      const z2 = parseZsig(fromHexT(fin.zsig));
      assert.equal(z2.version, 2);
      assert.equal(toHex(z2.pcztSha256), sha256hex(bytes));
      assert.equal(r.txid, 'ab'.repeat(32));
      assert.equal(host.state.lastResult.txid, r.txid);
      assert.equal(host.state.lastResult.seedFingerprint, k.seed_fingerprint);
      assert.ok(states.some((s) => s.task?.phase === 'review'));
      await host.run('disconnect');
      assert.equal(host.state.seedFingerprint, null);
    } finally {
      await host.run('disconnect');
      z.stop();
    }
  });

  test('another wallet on the board (a passphrase) has another fingerprint', async () => {
    const z = await startZdev({ yes: true, passphrase: 'leekwallet-other-board' });
    const host = new BoardHost({ engine: fakeEngine('test').engine, openLink: async () => new PtyLink(z.link) });
    try {
      await host.run('connect', { kind: 'usb', network: 'test' });
      assert.equal(host.state.phase, 'ready');
      assert.match(host.state.seedFingerprint, /^[0-9a-f]{64}$/);
      assert.notEqual(host.state.seedFingerprint, key('test/abandon-art/pp0/acct0').seed_fingerprint);
    } finally {
      await host.run('disconnect');
      z.stop();
    }
  });

  test('a USB timeout says what to check, closes the port, and Bluetooth connects next', async () => {
    const z = await startZdev({ yes: true });
    const port = silentSerialPort();
    const host = new BoardHost({
      engine: fakeEngine('test').engine,
      async openLink(kind, onClose) {
        if (kind === 'ble') return BleLink.open(fakeBluetoothDevice(z.link), onClose);
        // The board set to Bluetooth: the serial port opens and nothing answers.
        const serial = await SerialLink.open(port, onClose);
        return { kind: 'usb', label: serial.label, send: (t, p) => serial.send(t, p), recv: () => serial.recv(300), drain: () => serial.drain(), close: () => serial.close() };
      },
    });
    try {
      const hint = /did not answer over USB within \d+ s\. Is it unlocked, and is its Link set to USB \(Settings → Link\)\?/;
      await assert.rejects(host.run('connect', { kind: 'usb', network: 'test' }), hint);
      assert.equal(host.state.phase, 'disconnected');
      assert.match(host.state.error, hint);
      assert.ok(port.closed, 'the serial port is released');
      // Nothing is left running: Bluetooth goes at once.
      await host.run('connect', { kind: 'ble', network: 'test' });
      assert.equal(host.state.phase, 'ready');
      assert.equal(host.state.kind, 'ble');
      assert.equal(host.state.error, null);
      assert.equal(host.state.seedFingerprint, key('test/abandon-art/pp0/acct0').seed_fingerprint);
    } finally {
      await host.run('disconnect');
      z.stop();
    }
  });

  test('mainnet needs the real-funds confirmation; the engine must be on the same network', async () => {
    const z = await startZdev({ yes: true });
    const { engine, calls } = fakeEngine('test');
    const host = new BoardHost({ engine, openLink: async () => new PtyLink(z.link) });
    try {
      await host.run('connect', { kind: 'usb' });
      const view = toHex(vector('main_iw_send_shielded_compact').bytes);
      await assert.rejects(host.run('signAndFinish', { network: 'main', signerView: view }), /real ZEC/);
      await assert.rejects(host.run('signAndFinish', { network: 'main', signerView: view, confirmMainnet: true }), /engine is test, not main/);
      assert.equal(calls.filter((c) => c.method === 'finish').length, 0);
      assert.equal(host.state.phase, 'ready');
    } finally {
      await host.run('disconnect');
      z.stop();
    }
  });

  test('a board set to the other network: connected, no wallet shown, and the popup says which setting to change', async () => {
    const z = await startZdev({ yes: true, network: 'test' });
    const host = new BoardHost({ engine: fakeEngine('main').engine, openLink: async () => new PtyLink(z.link) });
    try {
      await host.run('connect', { kind: 'usb', network: 'main' });
      assert.equal(host.state.phase, 'ready');
      assert.equal(host.state.seedFingerprint, null);
      assert.equal(host.state.accountError, wrongNetworkMessage('main'));
      await assert.rejects(host.run('importAccount', { network: 'main' }), (e) => e.code === ERR.BOARD_NETWORK && e.message === wrongNetworkMessage('main'));
      assert.equal(host.state.error, wrongNetworkMessage('main'));
      await z.switchNetwork('main');
      await host.run('connect', { kind: 'usb', network: 'main' });
      assert.equal(host.state.seedFingerprint, key('main/abandon-art/pp0/acct0').seed_fingerprint);
      assert.equal(host.state.accountError, null);
    } finally {
      await host.run('disconnect');
      z.stop();
    }
  });
});

/** A Web Serial port whose board serves the other link: it opens, and never answers. */
function silentSerialPort() {
  return {
    closed: false,
    getInfo: () => ({ usbVendorId: 0x303a, usbProductId: 0x1001 }),
    async open() {
      this.readable = new ReadableStream({ pull: () => new Promise(() => {}) });
      this.writable = new WritableStream({ write() {} });
    },
    async setSignals() {},
    async close() {
      this.closed = true;
    },
  };
}

describe('link timeouts and Web Bluetooth help', () => {
  test('a timeout names the link and its setting on the board', () => {
    const e = linkTimeoutHint(new LinkTimeout(10e3), 'Bluetooth');
    assert.ok(e instanceof LinkTimeout);
    assert.equal(e.message, 'the board did not answer over Bluetooth within 10 s. Is it unlocked, and is its Link set to Bluetooth (Settings → Link)?');
    const other = new Error('x');
    assert.equal(linkTimeoutHint(other, 'USB'), other);
  });

  test('how to turn Web Bluetooth on: Brave, Chrome on Linux, elsewhere', () => {
    const linux = 'Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/152.0.0.0 Safari/537.36';
    const brave = bluetoothHelp({ userAgent: linux, brave: { isBrave: async () => true } });
    assert.match(brave, /brave:\/\/flags\/#brave-web-bluetooth-api/);
    assert.match(brave, /brave:\/\/flags\/#enable-experimental-web-platform-features/);
    const chromeLinux = bluetoothHelp({ userAgent: linux });
    assert.match(chromeLinux, /chrome:\/\/flags\/#enable-experimental-web-platform-features/);
    const win = 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/152.0.0.0 Safari/537.36';
    assert.doesNotMatch(bluetoothHelp({ userAgent: win, brave: { isBrave: async () => true } }), /experimental/);
    assert.match(bluetoothHelp({ userAgent: win }), /no Web Bluetooth/);
  });
});

describe('against zdev over Bluetooth (fake GATT, MTU 23)', { skip: !zdevOk && 'zdev could not be built' }, () => {
  test('pair, account, and a compact send signed', async () => {
    const z = await startZdev({ yes: true });
    const gattDevice = fakeBluetoothDevice(z.link);
    const link = await BleLink.open(gattDevice);
    const dev = new Device(link);
    try {
      await dev.pair();
      await dev.ensureUnlocked();
      const a = await dev.zGetAccount(0, 'test');
      assert.equal(a.ua, key('test/abandon-art/pp0/acct0').ua);
      const { v, bytes } = vector('t_mixed_compact');
      const r = await dev.zSign(bytes, 0, 'test');
      assert.equal(toHex(r.sighash), v.sighash);
      assert.ok(gattDevice.stats.maxWrite <= 20, `writes of ${gattDevice.stats.maxWrite} bytes`);
      assert.ok(gattDevice.stats.writes > bytes.length / 19);
    } finally {
      await link.close();
      z.stop();
    }
  });
});
