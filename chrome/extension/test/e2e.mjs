// End-to-end test of the extension in headless Chromium (Playwright).
//
//   cd chrome/extension && npm install && node build.mjs && node test/e2e.mjs
//
// It loads dist/ unpacked, opens the popup as a tab, and through the same
// EngineClient the popup uses:
//   1. starts the engine (threads, OPFS) and wipes the testnet wallet;
//   2. imports the abandon-art testnet account 0 UFVK (vectors/keys.json)
//      as a device account, birthday BIRTHDAY (default 4,501,000, before
//      the 10,000-zat Ironwood note found in 9.1);
//   3. starts a sync, stops it after the first scanned batch, and checks
//      the next sync carries on from there;
//   4. reloads the extension in the middle of a sync, then finishes it;
//   5. reads balances, addresses (and rotates one), history;
//   6. reloads the extension and checks the wallet came back from OPFS:
//      same account, same balances, same scan height, and a sync after
//      it scans only the new blocks;
//   7. restarts the whole browser (same profile) and checks it again.
//   5b. the board, as zdev (firmware/sim): pairs from an extension page,
//       imports the account from it (birthday asked, then recorded on the
//       board), checks a rotated address on it, refuses a foreign one.
// Opt-in, needs funds on the abandon-art testnet account (public keys, so
// anyone may have spent them): BROADCAST=1 also proposes a send to self,
// signs it with the dev soft signer, proves, extracts and broadcasts it;
// BROADCAST=board does the same with zdev signing through the board code.
//
// Env: BIRTHDAY, LWD (gRPC-web endpoint), HEADED=1, KEEP=1 (keep profile).
import { chromium } from 'playwright-core';
import { existsSync, mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { PtyLink, startZdev } from './zdev.mjs';

const here = dirname(fileURLToPath(import.meta.url));
const dist = join(here, '..', 'dist');
const repo = join(here, '..', '..', '..');
if (!existsSync(join(dist, 'manifest.json'))) {
  console.error('no dist/: run node build.mjs first');
  process.exit(1);
}
const keys = JSON.parse(readFileSync(join(repo, 'vectors/keys.json'), 'utf8'));
const vector = keys.find((k) => k.name === 'test/abandon-art/pp0/acct0');
const BIRTHDAY = Number(process.env.BIRTHDAY ?? 4_501_000);
const LWD = process.env.LWD ?? '';

const profile = mkdtempSync(join(tmpdir(), 'leek-zcash-e2e-'));
const results = [];
let failures = 0;
function check(name, ok, detail = '') {
  results.push({ name, ok, detail });
  if (!ok) failures += 1;
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${name}${detail ? `: ${detail}` : ''}`);
}

async function launch() {
  const ctx = await chromium.launchPersistentContext(profile, {
    channel: 'chromium', // full Chromium, new headless: extensions load
    headless: !process.env.HEADED,
    args: [
      `--disable-extensions-except=${dist}`,
      `--load-extension=${dist}`,
      // Chromium 137+ ignores --load-extension unless this is off.
      '--disable-features=DisableLoadExtensionCommandLineSwitch',
    ],
  });
  let [sw] = ctx.serviceWorkers();
  if (!sw) sw = await ctx.waitForEvent('serviceworker');
  return { ctx, id: new URL(sw.url()).host };
}

/** Opens the popup as a tab and connects an EngineClient in it. */
async function openPage(ctx, id) {
  const page = await ctx.newPage();
  page.on('console', (m) => {
    if (process.env.VERBOSE) console.log(`  [page] ${m.text()}`);
  });
  await page.goto(`chrome-extension://${id}/popup.html?bare`);
  await page.evaluate(async () => {
    const { EngineClient } = await import('./src/engine-client.js');
    // The popup has its own client; the test uses a second one.
    window.t = await EngineClient.connect();
    window.events = [];
    window.t.onEvent((e) => window.events.push(e));
    window.hostState = await window.t.ready(180e3);
  });
  return page;
}

const call = (page, method, params = {}) =>
  page.evaluate(([m, p]) => window.t.call(m, p), [method, params]);

const ms = (t0) => `${((Date.now() - t0) / 1000).toFixed(1)} s`;

/** Reloads the extension; returns a fresh page with an engine. */
async function reloadExtension(ctx, id, page) {
  // The Reload button of chrome://extensions. Developer mode must be on:
  // without it Chromium disables a reloaded unpacked extension
  // ("unsupportedDeveloperExtension"), and chrome.runtime.reload() as well.
  const ext = await ctx.newPage();
  await ext.goto('chrome://extensions');
  await ext.evaluate(() => new Promise((r) => chrome.developerPrivate.updateProfileConfiguration({ inDeveloperMode: true }, r)));
  const err = await ext.evaluate(
    (id) => new Promise((r) => chrome.developerPrivate.reload(id, { failQuietly: true }, () => r(chrome.runtime.lastError?.message ?? null))),
    id,
  );
  if (err) throw new Error(`reload: ${err}`);
  await ext.close();
  await page.close().catch(() => {});
  // Every extension page closed; the extension comes back under the same id.
  const t0 = Date.now();
  for (;;) {
    await new Promise((r) => setTimeout(r, 500));
    const p = await ctx.newPage();
    const ok = await p
      .goto(`chrome-extension://${id}/popup.html?bare`, { timeout: 5000 })
      .then(() => p.evaluate(() => typeof chrome?.runtime?.connect === 'function'))
      .catch(() => false);
    await p.close().catch(() => {});
    if (ok) break;
    if (Date.now() - t0 > 60e3) throw new Error('the extension did not come back after a reload');
  }
  return openPage(ctx, id);
}

let { ctx, id } = await launch();
console.log(`extension ${id}, profile ${profile}`);
try {
  // 1. Engine up.
  let t0 = Date.now();
  let page = await openPage(ctx, id);
  const hs = await page.evaluate(() => window.hostState);
  check('engine starts (threads, OPFS)', hs.ready && hs.info.threads && hs.info.threadsInPool > 0, `${hs.info.threadsInPool} threads, start ${hs.info.startMs} ms, api v${hs.info.apiVersion}`);
  const isolated = await page.evaluate(() => self.crossOriginIsolated);
  check('extension pages are cross-origin isolated', isolated === true);

  await call(page, 'wipe', { network: 'test' });
  let st = await call(page, 'open', { network: 'test', server: LWD });
  check('opens an empty testnet wallet', st.open && st.network === 'test' && st.accounts.length === 0, `server ${st.server}`);
  const tip = await call(page, 'tip', {});
  check('gRPC-web tip', tip.height > BIRTHDAY, `${tip.chainName} ${tip.height} via ${tip.server} (${tip.vendor} ${tip.version})`);

  // 2. Import the viewing key as a device account.
  t0 = Date.now();
  const imp = await call(page, 'importAccount', {
    ufvk: vector.ufvk,
    name: 'abandon-art 0',
    birthday: BIRTHDAY,
    origin: { seedFingerprint: vector.seed_fingerprint, accountIndex: 0 },
  });
  check('imports the UFVK as a device account', imp.imported && imp.account.kind === 'device' && imp.account.birthday === BIRTHDAY, `${imp.account.uuid}, ZFP ${imp.account.shortFingerprint}, ${ms(t0)}`);
  const again = await call(page, 'importAccount', { ufvk: vector.ufvk, birthday: BIRTHDAY });
  check('importing the same UFVK again changes nothing', again.imported === false && again.account.uuid === imp.account.uuid);
  const uuid = imp.account.uuid;
  const ua0 = imp.addresses.unified;

  // 3. Stop a sync after its first batch, then resume.
  t0 = Date.now();
  const stopped = await page.evaluate(async () => {
    window.events = [];
    const p = window.t.call('sync', { batch: 1000 });
    while (!window.events.some((e) => e.phase === 'scanned')) await new Promise((r) => setTimeout(r, 100));
    await window.t.call('cancelSync');
    return p;
  });
  check('a sync stops on cancelSync', stopped.cancelled === true, `${stopped.scannedBlocks} blocks scanned before it stopped, ${ms(t0)}`);
  st = await call(page, 'status');
  check('the scan state of a stopped sync is kept', st.sync && st.sync.progressNum > 0, `progress ${st.sync?.progressNum}/${st.sync?.progressDen}`);

  // 4. Reload the extension in the middle of a sync.
  await page.evaluate(async () => {
    window.events = [];
    window.t.call('sync', { batch: 1000 }).catch(() => {});
    while (!window.events.some((e) => e.phase === 'scanned')) await new Promise((r) => setTimeout(r, 100));
  });
  const midScanned = await page.evaluate(() => window.events.filter((e) => e.phase === 'scanned').at(-1).scannedBlocks);
  page = await reloadExtension(ctx, id, page);
  st = await call(page, 'open', { network: 'test', server: LWD });
  check('the wallet survives a reload in the middle of a sync', st.existed && st.accounts.length === 1 && st.accounts[0].uuid === uuid, `${midScanned} blocks into that sync`);

  // Finish the sync.
  t0 = Date.now();
  const synced = await call(page, 'sync', { batch: 1000 });
  const syncMs = Date.now() - t0;
  const events = await page.evaluate(() => window.events.filter((e) => e.event === 'sync'));
  check('sync finishes', !synced.cancelled && synced.status?.fullyScanned >= synced.tip - 1, `${synced.scannedBlocks} blocks in ${(syncMs / 1000).toFixed(1)} s (${Math.round(synced.scannedBlocks / (syncMs / 1000))} blocks/s), tip ${synced.tip}, ${events.length} progress events`);
  check('sync reports progress', events.some((e) => e.phase === 'started') && events.some((e) => e.phase === 'scanned'));

  // 5. Balances, addresses, history.
  const bal = await call(page, 'balances', { account: uuid });
  const b = bal.balances;
  check('balances per pool', b && ['orchard', 'ironwood', 'transparent'].every((p) => typeof b[p].total === 'number'), `total ${b?.total} zat (orchard ${b?.orchard.total}, ironwood ${b?.ironwood.total}, transparent ${b?.transparent.total}), spendable ${b?.spendable}`);
  if (b && b.total === 0) console.log('  note: the public abandon-art account holds nothing at the moment (anyone can spend it)');
  const addrs = await call(page, 'addresses', { account: uuid });
  check('default address matches the import', addrs.unified === ua0 && addrs.unified.startsWith('utest1'), addrs.unified.slice(0, 24) + '…');
  const next = await call(page, 'nextAddress', { account: uuid });
  check('address rotation gives a new address', next.unified !== ua0 && next.diversifierIndex !== '0', `index ${next.diversifierIndex}`);
  const history = await call(page, 'history', { account: uuid, limit: 20 });
  check('history', Array.isArray(history), `${history.length} transactions`);
  const kind = await call(page, 'checkAddress', { address: next.unified });
  check('checkAddress', kind.kind === 'unified');
  st = await call(page, 'status');
  const before = { status: st.sync, balances: b, db: st.dbBytes };
  console.log(`  wallet database ${(st.dbBytes / 1e6).toFixed(2)} MB, extension database ${(st.leekDbBytes / 1e3).toFixed(1)} kB`);
  const mem = await page.evaluate(() => window.t.call('workerState').catch(() => null));

  // Spending: a proposal needs more than the fee. Shown, not required.
  if (b && b.spendable > 20_000) {
    const prop = await call(page, 'proposeSend', { account: uuid, to: ua0, amountZat: 1000, compact: true });
    check('proposes a send (compact signer view kept apart from the full PCZT)', prop.signerView.length > 0 && prop.sha256.length === 64, `fee ${prop.feeZat}, view ${prop.signerView.length / 2} B`);
  } else {
    const err = await call(page, 'proposeSend', { account: uuid, to: ua0, amountZat: 1000 }).then(() => null, (e) => String(e.message));
    check('propose without enough funds fails cleanly', err !== null, err?.slice(0, 100));
  }

  // 5b. The board (9.3), as zdev: the extension's own board code, in this
  // extension page, with WebCrypto and the vendored ChaCha20-Poly1305,
  // talking to the firmware's protocol over a bridge to zdev's PTY.
  const z = await startZdev({ yes: true });
  if (!z) {
    check('board: zdev', false, 'zdev could not be built (make -C firmware/sim build/zdev)');
  } else {
    try {
      const pty = new PtyLink(z.link);
      await page.exposeFunction('zdevSend', (type, payload) => pty.send(type, Uint8Array.from(payload)));
      await page.exposeFunction('zdevRecv', async (ms) => {
        const f = await pty.recv(ms);
        return { type: f.type, payload: Array.from(f.payload) };
      });
      await page.exposeFunction('zdevDrain', () => pty.drain());
      t0 = Date.now();
      const board = await page.evaluate(
        async ([birthday, rotated, foreign]) => {
          const { BoardHost } = await import('./src/board/host.js');
          const link = {
            kind: 'usb',
            label: 'zdev',
            send: (type, payload) => window.zdevSend(type, Array.from(payload)),
            recv: async (ms) => {
              const f = await window.zdevRecv(ms);
              return { type: f.type, payload: new Uint8Array(f.payload) };
            },
            drain: () => window.zdevDrain(),
            close: async () => {},
          };
          const states = [];
          window.board = new BoardHost({ engine: (m, p) => window.t.call(m, p), openLink: async () => link, onState: (s) => states.push(s) });
          const out = {};
          await window.board.run('connect', { kind: 'usb' });
          out.passkey = states.find((s) => s.phase === 'pairing')?.passkey;
          out.phase = window.board.state.phase;
          out.needs = await window.board.run('importAccount', { network: 'test' }).then(() => null, (e) => e.need ?? e.message);
          out.imp = await window.board.run('importAccount', { network: 'test', birthday });
          out.again = await window.board.run('importAccount', { network: 'test' });
          out.shown = await window.board.run('showAddress', { network: 'test', ua: rotated });
          out.foreign = await window.board.run('showAddress', { network: 'test', ua: foreign }).then(() => null, (e) => e.message);
          return out;
        },
        [BIRTHDAY, next.unified, keys.find((k) => k.name === 'test/abandon-art/pp0/acct1').ua],
      );
      check('board: pairs with zdev from an extension page (WebCrypto X25519, vendored ChaCha20-Poly1305)', board.phase === 'ready' && board.passkey === z.passkeys.at(-1), `passkey ${board.passkey}, ${ms(t0)}`);
      check('board: no birthday on the board yet, so it asks', board.needs === 'birthday');
      check('board: zGetAccount imports as the same device account (same UFVK and origin)', board.imp.account.uuid === uuid && board.imp.imported === false && board.imp.board.seedFingerprint === vector.seed_fingerprint && board.imp.storedOnBoard === true, `ZFP ${board.imp.account.shortFingerprint}`);
      check("board: the next import takes the board's birthday", board.again.birthdayFromBoard && board.again.birthday === BIRTHDAY);
      check("board: the engine's default address is the board's", board.imp.addresses.unified === board.imp.board.ua);
      check('board: zShowAddress confirms a rotated address', board.shown.ua === next.unified, `diversifier index ${next.diversifierIndex}`);
      check('board: zShowAddress refuses an address not its own', !!board.foreign, board.foreign?.slice(0, 80));
      if (process.env.BROADCAST === 'board' && b && b.spendable > 20_000) {
        t0 = Date.now();
        const sent = await page.evaluate(async (to) => {
          const prop = await window.t.call('proposeSend', { to, amountZat: 1000, memo: 'leekwallet chrome 9.3 (zdev)', compact: true });
          return window.board.run('signAndFinish', { network: 'test', accountIndex: 0, signerView: prop.signerView, broadcast: true });
        }, ua0);
        check('board: a send signed by zdev, proven, extracted and broadcast', sent.broadcast && sent.txid?.length === 64, `txid ${sent.txid}, ${ms(t0)}`);
      }
      await pty.close();
    } catch (e) {
      check('board: no exception', false, String(e?.stack ?? e));
    } finally {
      z.stop();
    }
  }

  // 6. Reload the extension: the wallet comes back from OPFS.
  page = await reloadExtension(ctx, id, page);
  st = await call(page, 'open', { network: 'test', server: LWD });
  const bal2 = await call(page, 'balances', { account: uuid });
  check('after a reload: same account', st.existed && st.accounts.length === 1 && st.accounts[0].uuid === uuid);
  check('after a reload: same scan height', st.sync?.fullyScanned === before.status.fullyScanned, `${st.sync?.fullyScanned}`);
  check('after a reload: same balances', JSON.stringify(bal2.balances) === JSON.stringify(before.balances));
  const rotated = await call(page, 'nextAddress', { account: uuid });
  check('after a reload: address rotation continues', rotated.diversifierIndex !== next.diversifierIndex && rotated.unified !== next.unified, `index ${rotated.diversifierIndex}`);
  t0 = Date.now();
  const resync = await call(page, 'sync', {});
  check('after a reload: sync scans only new blocks', !resync.cancelled && resync.scannedBlocks < 200, `${resync.scannedBlocks} blocks, ${ms(t0)}`);

  // 7. Restart the browser with the same profile.
  await ctx.close();
  ({ ctx, id } = await launch());
  page = await openPage(ctx, id);
  st = await call(page, 'open', { network: 'test', server: LWD });
  const bal3 = await call(page, 'balances', { account: uuid });
  check('after a browser restart: wallet, scan height and balances persisted', st.existed && st.accounts[0]?.uuid === uuid && st.sync?.fullyScanned >= before.status.fullyScanned && bal3.balances?.total === before.balances?.total, `scanned to ${st.sync?.fullyScanned}, total ${bal3.balances?.total}`);
  const pend = await call(page, 'pending');
  check('pending sends persisted', Array.isArray(pend), `${pend.length} pending`);

  // Mainnet opens beside it, in its own files.
  const main = await call(page, 'open', { network: 'main' });
  check('mainnet opens in its own database', main.network === 'main' && main.accounts.length === 0, main.server);
  await call(page, 'open', { network: 'test', server: LWD });

  // The popup itself renders the wallet.
  const popup = await ctx.newPage();
  await popup.goto(`chrome-extension://${id}/popup.html?tab`);
  popup.on('console', (m) => console.log(`  [popup] ${m.text()}`));
  popup.on('pageerror', (e) => console.log(`  [popup error] ${e.message}`));
  await popup.waitForSelector('#total', { timeout: 120e3 }).catch(async (e) => {
    console.log(`  popup shows: ${await popup.evaluate(() => document.body.innerText)}`);
    throw e;
  });
  const shown = await popup.evaluate(() => ({ total: document.getElementById('total')?.textContent, ua: document.getElementById('ua')?.textContent }));
  check('the popup shows the balance and an address', !!shown.total && shown.ua?.startsWith('utest1'), `${shown.total}`);
  // The board in the popup: the device-account screens are there, and
  // "Connect over USB" with no port granted (headless: none exists) asks
  // for the browser's chooser, which a tab can open.
  const boardUi = await popup.evaluate(() => ({ board: !!document.getElementById('board'), send: !!document.getElementById('send'), verify: !!document.getElementById('verify') }));
  check('the popup shows the board, send and verify controls for a device account', boardUi.board && boardUi.send && boardUi.verify);
  await popup.click('#connect-usb');
  const granted = await popup.waitForSelector('#grant-usb', { timeout: 30e3 }).then(() => true, () => false);
  check('USB with no port granted: the offscreen document asks for the chooser', granted);
  await popup.screenshot({ path: join(tmpdir(), 'leek-zcash-popup.png'), fullPage: true });
  await popup.close();

  // Opt-in: a real send, broadcast.
  if (process.env.BROADCAST && process.env.BROADCAST !== 'board') {
    t0 = Date.now();
    const out = await page.evaluate(async ([to, mnemonic]) => {
      const t = window.t;
      const prop = await t.call('proposeSend', { to, amountZat: 1000, memo: 'leekwallet chrome 9.2', compact: true });
      const { zsig } = await t.call('signSoft', { signerView: prop.signerView, mnemonic });
      window.events = [];
      const fin = await t.call('finish', { signerView: prop.signerView, zsig, broadcast: true });
      return { prop, fin, events: window.events };
    }, [ua0, vector.mnemonic]);
    check('broadcast: propose, sign (soft), prove, extract, send', out.fin.broadcast && out.fin.txid.length === 64, `txid ${out.fin.txid}, ${out.fin.size} B, prove ${Math.round(out.fin.proveMs)} ms, ${ms(t0)}`);
  } else {
    console.log('skip broadcast (opt-in: BROADCAST=1, needs funds on the abandon-art testnet account)');
  }
  if (mem?.memoryMb) console.log(`  WASM memory ${mem.memoryMb.toFixed(0)} MB`);
} catch (e) {
  check('no exception', false, String(e?.stack ?? e));
} finally {
  await ctx.close().catch(() => {});
  if (!process.env.KEEP) rmSync(profile, { recursive: true, force: true });
}
console.log(`\n${results.length - failures}/${results.length} checks passed`);
process.exit(failures ? 1 : 0);
