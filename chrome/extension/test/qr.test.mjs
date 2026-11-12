// Sign with QR and Import from the board's QR, in Node (no browser, no
// board): the leek-ur WASM module (chrome/ur/pkg) the pages load, the QR
// codes drawn with the vendored qrcode-generator and read back with the
// vendored zxing-wasm reader (the webcam's decoder), and the state
// machines of src/qr/.
//
//   node --test test/qr.test.mjs      (needs chrome/ur/build.sh and npm install)
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { before, describe, test } from 'node:test';
import { vendor } from '../vendor.mjs';
import { enc, loadUrNode, record } from './qr-fixtures.mjs';

await vendor();

const here = dirname(fileURLToPath(import.meta.url));
const repo = join(here, '..', '..', '..');
const vec = (p) => new Uint8Array(readFileSync(join(repo, 'vectors', p)));
const fixtureParts = (p) =>
  readFileSync(join(repo, 'vectors', p), 'utf8')
    .split('\n')
    .filter((l) => l && !l.startsWith('#'));

const ur = await loadUrNode();

const { loadUr, hex, fromHex } = await import('../src/qr/ur.js');
const { drawQr, qrPixels, qrVersionFor, modulesFor, moduleScale, qrAvail } = await import('../src/qr/qr-code.js');
const { QrSigner, QR_PRESETS, QR_SETTINGS_KEY, frameCaption, scanCaption, presetsFromHints } = await import('../src/qr/sign.js');
const { QrAccountImport, checkRecord, decodeAccountRecord, resolveBirthday, ufvkNetwork, BIRTHDAY_MARGIN, ACCOUNT_RECORD_UR_TYPE, accountsCaption } = await import('../src/qr/accounts.js');
const { reader, decodePasses, centerCrop, enhance, nextCamera, loadCamera, saveCamera, cameraError } = await import('../src/qr/scan.js');
const qrcode = (await import('../src/vendor/qrcode-generator/qrcode.mjs')).default;

let zxing;
before(async () => {
  zxing = await reader({ wasmBinary: readFileSync(join(here, '..', 'src/vendor/zxing-wasm/reader/zxing_reader.wasm')) });
});

/** A frame as the board's camera (or the webcam) would see it, decoded by zxing. */
async function seen(text, { version = 0, mode = 'Alphanumeric', px = 3 } = {}) {
  const img = qrPixels(qrcode, text, { version, mode, px, quiet: 2 });
  const found = await zxing.readBarcodes({ data: img.data, width: img.width, height: img.height }, { formats: ['QRCode'], tryHarder: true });
  assert.equal(found.length, 1, `one code in ${text.slice(0, 30)}`);
  return { text: found[0].text, modules: img.modules };
}

function fakeClock() {
  let t = 0;
  let next = 1;
  const timers = new Map();
  return {
    set(f, ms) {
      const id = next++;
      timers.set(id, { f, at: t + ms });
      return id;
    },
    clear(id) {
      timers.delete(id);
    },
    now: () => t,
    pending: () => timers.size,
    /** Run the earliest timer. */
    tick() {
      const [id, e] = [...timers.entries()].sort((a, b) => a[1].at - b[1].at)[0] ?? [];
      if (!id) return false;
      timers.delete(id);
      t = e.at;
      e.f();
      return true;
    },
  };
}

function memStore() {
  const m = new Map();
  return { getItem: (k) => (m.has(k) ? m.get(k) : null), setItem: (k, v) => m.set(k, String(v)), map: m };
}

const keys = JSON.parse(readFileSync(join(repo, 'vectors/keys.json'), 'utf8'));
const testKey = keys.find((k) => k.name === 'test/abandon-art/pp0/acct0');
const mainKey = keys.find((k) => k.name === 'main/abandon-art/pp0/acct0');

// ------------------------------------------------------------- tests

describe('leek-ur in WASM (chrome/ur)', () => {
  test('loadUr hands back the module it is given', async () => {
    assert.equal(await loadUr(ur), ur);
  });

  test('the ur:leek-pczt loop is the shared fixture, uppercased', () => {
    const view = vec('pczt/iw_send_nu7_compact.pczt');
    const e = ur.UrEncoder.leekPczt(view, 50);
    assert.equal(e.seqLen, 28);
    assert.equal(e.fragmentLen, 50);
    assert.ok(!e.singlePart);
    const parts = fixtureParts('ur/iw_send_nu7_compact.50.leek-pczt.txt');
    parts.forEach((p, i) => assert.equal(e.frame(i), p.toUpperCase()));
    for (let i = 0; i < 60; i++) {
      const f = e.frame(i);
      assert.ok(f.length <= e.longest);
      assert.match(f, /^[0-9A-Z $%*+\-./:]+$/, 'QR alphanumeric');
    }
    assert.throws(() => ur.UrEncoder.leekPczt(new TextEncoder().encode('not a pczt'), 50), /not a PCZT/);
  });

  test('the display hints are leek-ur’s laptop ones', () => {
    const h = ur.displayHints();
    assert.deepEqual([h.fragmentLen, h.frameMs, h.boardZoom, h.quietZoneModules], [50, 300, 2, 2]);
    assert.ok(ur.largeScreenTips().some((t) => t.includes('2x')));
    assert.equal(ur.pageBackground(), '#000000');
  });

  test('the board’s reply assembles from any order; other codes are told apart', () => {
    const zsig = vec('zsig/iw_send_shielded.zsig');
    const s = new ur.UrScan('leek-zsig');
    assert.equal(s.feed('zcash:t1abc'), 'ignored');
    assert.equal(s.feed('UR:LEEK-PCZT/1-28/AEAEAEAE'), 'ignored');
    assert.equal(s.feed('UR:CRYPTO-PSBT/AEAEAEAE'), 'rejected');
    assert.match(s.error, /ur:crypto-psbt/);
    assert.equal(s.total, 0, 'nothing pinned by a foreign code');
    let done = null;
    for (const p of fixtureParts('ur/iw_send_shielded.30.leek-zsig.txt').reverse()) {
      const st = s.feed(p.toUpperCase());
      assert.notEqual(st, 'rejected', s.error);
      if (st === 'complete') {
        done = s.takeMessage();
        break;
      }
      assert.equal(s.total, 4);
    }
    assert.deepEqual(done, zsig);
    assert.equal(s.takeMessage(), undefined, 'handed over once');
    assert.equal(s.total, 0, 'starts over');
    const one = fixtureParts('ur/iw_send_shielded.512.leek-zsig.txt')[0];
    assert.equal(s.feed(one), 'complete');
    assert.deepEqual(s.takeMessage(), zsig);
    assert.throws(() => new ur.UrScan('crypto-psbt'));
  });
});

describe('QR codes for the board, read back as the camera reads them', () => {
  test('qrVersionFor and the module maths', () => {
    assert.equal(qrVersionFor(25), 1);
    assert.equal(qrVersionFor(26), 2);
    assert.equal(qrVersionFor(4296), 40);
    assert.throws(() => qrVersionFor(5000));
    assert.equal(modulesFor(6), 41);
    assert.deepEqual(moduleScale(45, 500), { px: 11, side: 495 });
    assert.equal(qrAvail(1200, 800, 260), 540);
  });

  test('a compact send’s loop: every frame one QR version, read back whole', async () => {
    const view = vec('pczt/iw_send_nu7_compact.pczt');
    for (const fragment of [50, 40]) {
      const e = ur.UrEncoder.leekPczt(view, fragment);
      const version = qrVersionFor(e.longest);
      const back = new ur.UrScan('leek-pczt');
      let state = null;
      let frames = 0;
      for (let i = 0; i < e.seqLen * 3 && state !== 'complete'; i++) {
        const f = e.frame(i);
        const r = await seen(f, { version });
        assert.equal(r.text, f);
        assert.equal(r.modules, modulesFor(version), 'the code keeps its size');
        state = back.feed(r.text);
        frames++;
      }
      assert.equal(state, 'complete', `${fragment} B`);
      assert.deepEqual(back.takeMessage(), view);
      console.log(`  ${fragment} B: ${e.seqLen} fragments, QR v${version}, read back in ${frames} frames`);
    }
  });

  test('drawQr: whole pixels per module, a 2-module white quiet zone', () => {
    const ops = [];
    const ctx = { fillRect: (...a) => ops.push(['rect', ...a]), set fillStyle(v) { ops.push(['fill', v]); } };
    const canvas = { width: 0, height: 0, style: {}, getContext: () => ctx };
    const n = drawQr(canvas, qrcode, 'UR:LEEK-PCZT/1-28/ABC', { version: 6, avail: 300, dpr: 2, quiet: 2 });
    assert.equal(n, 41);
    const px = Math.floor(600 / 45);
    assert.equal(canvas.width, px * 45);
    assert.equal(canvas.style.width, `${(px * 45) / 2}px`);
    const rects = ops.filter((o) => o[0] === 'rect').slice(1);
    assert.ok(rects.every(([, x, y, w]) => w === px && x >= 2 * px && y >= 2 * px && x % px === 0));
    assert.deepEqual(ops[0], ['fill', '#ffffff']);
  });

  test('an address in byte mode keeps its case (Check address)', async () => {
    const r = await seen(testKey.ua, { mode: 'Byte' });
    assert.equal(r.text, testKey.ua);
  });
});

describe('Sign with QR (src/qr/sign.js)', () => {
  const view = vec('pczt/iw_send_nu7_compact.pczt');

  test('the loop advances on the clock; presets restart it or change the pace', () => {
    const clock = fakeClock();
    const storage = memStore();
    const q = new QrSigner(ur, { clock, storage, hints: ur.displayHints(), versionFor: qrVersionFor });
    let changes = 0;
    q.onChange = () => changes++;
    assert.equal(q.preset, 'laptop');
    assert.deepEqual([q.settings.fragment, q.settings.interval], [50, 300]);
    q.show(view);
    assert.equal(q.phase, 'showing');
    let f = q.frame();
    assert.equal(f.part, '1-28');
    assert.equal(f.version, 6);
    assert.match(frameCaption(f), /^Part 1-28 · frame 1 · 28 fragments of 50 B · QR version 6 · 300 ms$/);
    clock.tick();
    clock.tick();
    f = q.frame();
    assert.equal(f.part, '3-28');
    assert.equal(f.shown, 3);
    assert.equal(clock.now(), 600);
    assert.equal(clock.pending(), 1, 'one timer at a time');
    q.setPreset('smaller');
    f = q.frame();
    assert.equal(f.part.split('-')[0], '1', 'a new fragment size starts at part 1');
    assert.equal(f.fragmentLen, 40);
    assert.equal(storage.map.get(QR_SETTINGS_KEY), 'smaller');
    q.setPreset('laptop');
    clock.tick();
    q.setPreset('safe');
    assert.equal(q.frame().part, '2-28', 'same fragments at another pace');
    const t0 = clock.now();
    clock.tick();
    assert.equal(clock.now() - t0, 500);
    assert.equal(clock.pending(), 1);
    assert.ok(changes > 5);
    // Remembered for the next signing.
    assert.equal(new QrSigner(ur, { clock, storage }).preset, 'safe');
    q.cancel();
    assert.equal(clock.pending(), 0);
    assert.equal(q.frame(), null);
  });

  test('the presets: laptop from the hints, a 40 B one, and Safe', () => {
    assert.deepEqual(
      Object.values(QR_PRESETS).map((p) => [p.fragment, p.interval]),
      [
        [50, 300],
        [40, 300],
        [50, 500],
      ],
    );
    assert.equal(presetsFromHints({ fragmentLen: 49, frameMs: 350 }).laptop.fragment, 49);
    assert.equal(new QrSigner(ur, { storage: { getItem: () => 'bogus', setItem() {} } }).preset, 'laptop');
  });

  test('scanning: the board’s reply, read off its QR codes, ends in the .zsig', async () => {
    const clock = fakeClock();
    const q = new QrSigner(ur, { clock, storage: memStore() });
    q.show(view);
    q.startScan();
    assert.equal(q.phase, 'scanning');
    assert.equal(clock.pending(), 0, 'the loop stopped');
    assert.match(scanCaption(q.progress(), q.note), /Point the webcam/);
    assert.equal(q.feed('zcash:utest1xyz'), null);
    assert.equal(q.feed(q.phase === 'scanning' ? 'UR:LEEK-PCZT/1-28/AEAEAEAE' : ''), null);
    assert.equal(q.feed('UR:CRYPTO-PSBT/AEAEAEAE'), null);
    assert.match(q.note, /ur:crypto-psbt/);
    // The board's screen, through the webcam's decoder.
    const zsig = vec('zsig/iw_send_shielded.zsig');
    const board = ur.UrEncoder.leekZsig(zsig, 30);
    let got = null;
    for (let i = 0; i < 20 && !got; i++) {
      const r = await seen(board.frame(i), { version: qrVersionFor(board.longest) });
      got = q.feed(r.text);
      if (!got) assert.match(scanCaption(q.progress(), q.note), /^Signature: \d of 4 parts read…/);
    }
    assert.deepEqual(got, zsig);
    assert.equal(q.phase, 'finishing');
    assert.equal(q.feed(board.frame(0)), null, 'nothing after the reply is whole');
    q.finished();
    assert.equal(q.phase, 'done');
  });

  test('back to the loop from the scan, and a failure', () => {
    const clock = fakeClock();
    const q = new QrSigner(ur, { clock, storage: memStore() });
    q.show(view);
    q.startScan();
    q.showAgain();
    assert.equal(q.phase, 'showing');
    assert.equal(q.frame().part, '1-28');
    q.fail(new Error('the unsigned PCZT does not match the pending one (sighash differs)'));
    assert.equal(q.phase, 'failed');
    assert.match(q.error, /sighash differs/);
    assert.equal(clock.pending(), 0);
    assert.throws(() => q.show('not bytes'));
  });
});

describe('Import from the board’s QR (src/qr/accounts.js)', () => {
  const fp = fromHex(testKey.seed_fingerprint);

  test('the record reads as record.rs writes it, tagged or not', () => {
    for (const tagged of [false, true]) {
      const r = decodeAccountRecord(record({ fp, ufvk: testKey.ufvk, name: 'Leek', birthday: 4506700, tagged }));
      assert.equal(r.seedFingerprint, testKey.seed_fingerprint);
      assert.deepEqual(r.accounts, [{ ufvk: testKey.ufvk, index: 0, name: 'Leek', birthday: 4506700 }]);
    }
    const plain = decodeAccountRecord(record({ fp, ufvk: testKey.ufvk }));
    assert.deepEqual(plain.accounts[0], { ufvk: testKey.ufvk, index: 0, name: null, birthday: null });
    assert.equal(ufvkNetwork(testKey.ufvk), 'test');
    assert.equal(ufvkNetwork(mainKey.ufvk), 'main');
    assert.equal(ufvkNetwork('zxviews1abc'), null);
  });

  test('malformed records and another network are refused', () => {
    assert.throws(() => decodeAccountRecord(new Uint8Array([0xa0])), /seed fingerprint/);
    assert.throws(() => decodeAccountRecord(record({ fp: fp.slice(0, 31), ufvk: testKey.ufvk })), /seed fingerprint/);
    assert.throws(() => decodeAccountRecord(record({ fp, ufvk: 'not a key' })), /viewing key/);
    assert.throws(() => decodeAccountRecord(record({ fp, ufvk: testKey.ufvk, index: 0x80000000 })), /out of range/);
    const extra = new Uint8Array([...record({ fp, ufvk: testKey.ufvk }), 0]);
    assert.throws(() => decodeAccountRecord(extra), /after the record/);
    // A .zsig-style byte string around it is not the record.
    assert.throws(() => checkRecord('test', new Uint8Array(enc.bytes([...record({ fp, ufvk: testKey.ufvk })]))), /not a readable account record/);
    const main = record({ fp: fromHex(mainKey.seed_fingerprint), ufvk: mainKey.ufvk });
    assert.throws(() => checkRecord('test', main), /this is a Mainnet viewing key, and the Testnet wallet is chosen here: pick Mainnet and scan again/);
    const ok = checkRecord('test', record({ fp, ufvk: testKey.ufvk }));
    assert.equal(ok.zfp, testKey.seed_fingerprint.slice(0, 8).toUpperCase());
  });

  test('the birthday: typed, the record’s, or the tip minus the margin', async () => {
    const tip = async () => ({ height: 5_000_000 });
    assert.equal(await resolveBirthday(' 4506700 ', null, tip), 4506700);
    assert.equal(await resolveBirthday('', 4506700, tip), 4506700);
    assert.equal(await resolveBirthday('', null, tip), 5_000_000 - BIRTHDAY_MARGIN);
    await assert.rejects(resolveBirthday('12a', null, tip), /digits only/);
  });

  test('the View key loop, through the webcam’s decoder, imports a device account', async () => {
    const rec = record({ fp, ufvk: testKey.ufvk });
    const e = new ur.UrEncoder(ACCOUNT_RECORD_UR_TYPE, rec, 50);
    assert.ok(e.seqLen > 4, 'multi-part');
    const q = new QrAccountImport(ur);
    q.start('test');
    assert.equal(q.feed('UR:LEEK-ZSIG/AEAEAEAE'), false, 'the board’s signature is not its key');
    assert.equal(q.feed('UR:ZCASH-ACCOUNTS/AEAEAEAE'), false);
    assert.match(q.note, /ur:zcash-accounts/);
    let stop = false;
    for (let i = 0; i < e.seqLen * 3 && !stop; i++) {
      const r = await seen(e.frame(i), { version: qrVersionFor(e.longest) });
      stop = q.feed(r.text.toLowerCase());
      if (!stop) assert.match(accountsCaption(q), /^\d+ of \d+ parts read/);
    }
    assert.equal(q.phase, 'found');
    assert.equal(q.record.zfp, testKey.seed_fingerprint.slice(0, 8).toUpperCase());
    const calls = [];
    const engine = async (m, p) => {
      calls.push([m, p]);
      if (m === 'tip') return { height: 4_600_000 };
      if (m === 'importAccount') return { imported: true, account: { uuid: 'u1' }, addresses: { unified: testKey.ua } };
      throw new Error(m);
    };
    const r = await q.importFound(engine, '');
    assert.equal(q.phase, 'done');
    assert.equal(r.birthday, 4_600_000 - BIRTHDAY_MARGIN);
    assert.deepEqual(calls.at(-1), [
      'importAccount',
      { ufvk: testKey.ufvk, birthday: 4_600_000 - BIRTHDAY_MARGIN, name: 'LeekWallet #0', origin: { seedFingerprint: testKey.seed_fingerprint, accountIndex: 0 } },
    ]);
  });

  test('a key of the other network stops the scan with the companion’s words', () => {
    const e = new ur.UrEncoder(ACCOUNT_RECORD_UR_TYPE, record({ fp: fromHex(mainKey.seed_fingerprint), ufvk: mainKey.ufvk }), 500);
    const q = new QrAccountImport(ur);
    q.start('test');
    assert.equal(q.feed(e.frame(0)), true);
    assert.equal(q.phase, 'failed');
    assert.match(q.error, /Mainnet viewing key/);
  });

  test('an import the engine refuses leaves the record found, to try again', async () => {
    const q = new QrAccountImport(ur);
    q.start('test');
    q.feed(new ur.UrEncoder(ACCOUNT_RECORD_UR_TYPE, record({ fp, ufvk: testKey.ufvk, birthday: 4506700 }), 500).frame(0));
    await assert.rejects(q.importFound(async () => {
      throw new Error('GetTreeState: unavailable');
    }));
    assert.equal(q.phase, 'found');
    assert.match(q.error, /GetTreeState/);
  });
});

describe('the webcam’s decoding passes (src/qr/scan.js)', () => {
  test('qr-tuning’s passes, crop and contrast', () => {
    assert.deepEqual(decodePasses(1, 0, true), [{ region: 'center', binarizer: 'LocalAverage', contrast: false }]);
    assert.equal(decodePasses(0, 0, true).length, 2);
    assert.equal(decodePasses(3, 0, false)[1].region, 'full');
    assert.deepEqual(centerCrop(1920, 1080), { sx: 636, sy: 216, size: 648 });
    const px = new Uint8ClampedArray([10, 10, 10, 255, 200, 200, 200, 255, 100, 100, 100, 255]);
    enhance(px, 0);
    assert.equal(px[0], 0);
    assert.equal(px[4], 255);
    assert.equal(px[3], 255);
  });

  test('cameras: remembered by id, the next one, the browser’s errors in words', () => {
    const s = memStore();
    assert.deepEqual(loadCamera(s), { any: true });
    saveCamera({ deviceId: 'cam2' }, s);
    assert.deepEqual(loadCamera(s), { deviceId: 'cam2' });
    assert.deepEqual(nextCamera([{ deviceId: 'a' }, { deviceId: 'b' }], 'a'), { deviceId: 'b' });
    assert.equal(nextCamera([{ deviceId: 'a' }], 'a'), null);
    assert.match(cameraError({ name: 'NotAllowedError' }), /not allowed/);
    assert.match(cameraError({ name: 'NotFoundError' }), /No camera/);
  });

  test('hex helpers', () => {
    assert.equal(hex(new Uint8Array([0, 255, 16])), '00ff10');
    assert.deepEqual(fromHex('00ff10'), new Uint8Array([0, 255, 16]));
    assert.throws(() => fromHex('0g'));
  });
});
