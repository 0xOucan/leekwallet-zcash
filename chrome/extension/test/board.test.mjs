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
const { Device, DeviceError, ERR, toHex } = await import('../src/board/device.js');
const { BleLink } = await import('../src/board/ble-link.js');
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
      const k = key(`${network}/abandon-art/pp0/acct0`);
      const a = await dev.zGetAccount(0, network);
      assert.equal(a.ufvk, k.ufvk, network);
      assert.equal(a.ua, k.ua, network);
      assert.equal(a.tAddress, k.transparent_address, network);
      assert.equal(a.seedFingerprint, k.seed_fingerprint, network);
    }
    const a1 = await dev.zGetAccount(1, 'test');
    assert.equal(a1.ufvk, key('test/abandon-art/pp0/acct1').ufvk);
  });

  test('zSetAccountMeta keeps one birthday per network', async () => {
    assert.equal((await dev.zGetAccount(0, 'test')).birthday, null);
    assert.equal(await dev.zSetBirthday(0, 'test', 4_501_000), true);
    assert.equal(await dev.zSetBirthday(0, 'main', 3_520_600), true);
    assert.equal((await dev.zGetAccount(0, 'test')).birthday, 4_501_000);
    assert.equal((await dev.zGetAccount(0, 'main')).birthday, 3_520_600);
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
    const r = await dev.zSign(bytes, 0, 'main');
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
