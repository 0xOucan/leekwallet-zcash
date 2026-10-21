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
//      4b. forgets the wallet's transparent spends (as if made by another
//      wallet) and checks the next sync finds them again;
//   5. reads balances, addresses (and rotates one), history;
//   6. reloads the extension and checks the wallet came back from OPFS:
//      same account, same balances, same scan height, and a sync after
//      it scans only the new blocks;
//   7. restarts the whole browser (same profile) and checks it again.
//   5b. the board, as zdev (firmware/sim): pairs from an extension page,
//       imports the account from it (birthday asked, then recorded on the
//       board), checks a rotated address on it, refuses a foreign one.
//   8. the popup: with no board it shows the board section only, nothing
//      of the board's wallet; connected over Bluetooth (a stand-in for
//      navigator.bluetooth whose GATT server is zdev) it shows the wallet,
//      and hides it again on Disconnect; a board with another wallet (zdev
//      with a passphrase) sees none of it and is offered its own import;
//      a passphrase typed in the popup opens the hidden wallet (XFP on the
//      board), imported as its own account, gone on Disconnect and not
//      back on reconnect (the board drops it with the session); "Connect
//      over USB" in the toolbar popup with no port granted opens a tab
//      that says why;
//      "Forget this board" deletes the account.
//   9. the board's wallet hidden in every view, on testnet and mainnet
//      (zdev set to that network): a device account just imported, its
//      balance not in yet ("No balance yet: sync first") and a sync
//      running; the tab connected over Bluetooth whose stack never answers
//      stopNotifications (as seen on Linux). Disconnect hides it at once;
//      a popup opened meanwhile, or reopened after, shows nothing of it;
//      the offscreen document holds no USB session; Forget hides it at
//      once while the sync runs, then deletes it.
//      SKIP_TO=9 runs only this step (with the engine started).
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
import { BleReassembler, chunkForBle, encodeFrame } from '../src/board/framing.js';
import { historyEntry } from '../src/format.js';

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

/**
 * Opens the popup as a tab whose navigator.bluetooth is a stand-in: its
 * one device's GATT server is zdev at `ptyPath`, reached through Node
 * (chunks at MTU 23 both ways, as the board's ble.c). The popup's own
 * code, BleLink included, runs unchanged.
 */
async function popupWithBluetooth(ctx, id, ptyPath, { hangStopNotifications = false } = {}) {
  const popup = await ctx.newPage();
  popup.on('pageerror', (e) => console.log(`  [popup error] ${e.message}`));
  const pty = new PtyLink(ptyPath);
  const reasm = new BleReassembler();
  await popup.exposeFunction('bleWrite', async (chunk) => {
    const f = reasm.push(Uint8Array.from(chunk));
    if (f) await pty.send(f.type, f.payload);
  });
  await popup.addInitScript((hang) => {
    const listeners = [];
    // hang: a Bluetooth stack that never answers stopNotifications.
    const rx = { addEventListener: (ev, f) => listeners.push(f), startNotifications: async () => rx, stopNotifications: () => (hang ? new Promise(() => {}) : Promise.resolve(rx)) };
    const tx = { writeValueWithResponse: (c) => window.bleWrite(Array.from(c)) };
    window.bleNotify = (bytes) => {
      const value = new DataView(Uint8Array.from(bytes).buffer);
      for (const f of listeners) f({ target: { value } });
    };
    const service = { getCharacteristic: async (uuid) => (uuid.endsWith('2') ? tx : rx) };
    window.gattDisconnects = 0;
    const device = { name: 'zdev', addEventListener() {}, gatt: { connect: async () => ({ getPrimaryService: async () => service }), disconnect: () => (window.gattDisconnects += 1) } };
    Object.defineProperty(navigator, 'bluetooth', { value: { requestDevice: async () => device }, configurable: true });
  }, hangStopNotifications);
  let open = true;
  (async () => {
    while (open) {
      let f;
      try {
        f = await pty.recv(3600e3);
      } catch {
        return;
      }
      for (const c of chunkForBle(encodeFrame(f.type, f.payload), 23)) await popup.evaluate((b) => window.bleNotify(b), Array.from(c)).catch(() => {});
    }
  })();
  await popup.goto(`chrome-extension://${id}/popup.html?tab&connect=ble`);
  popup.closeBridge = async () => {
    open = false;
    await pty.close();
    await popup.close().catch(() => {});
  };
  return popup;
}

/** Resolves with the ms it took for `f` (in the page) to hold, or null after `ms`. */
async function within(p, f, ms, arg) {
  const t0 = Date.now();
  const ok = await p.waitForFunction(f, arg, { timeout: ms, polling: 50 }).then(() => true, () => false);
  return ok ? Date.now() - t0 : null;
}

const WALLET_SHOWN = () => ['#account', '#total', '#ua', '#send', '#history', '#sync', '#balances'].some((s) => document.querySelector(s));

/**
 * 9. The board's wallet, in every view, while a sync runs: per network, a
 * device account just imported (a deep birthday, so the sync lasts), the
 * tab connected over Bluetooth whose stack never answers
 * stopNotifications.
 */
async function boardViews(ctx, id, page) {
  for (const network of ['test', 'main']) {
    const tag = network === 'main' ? 'mainnet' : 'testnet';
    const v = keys.find((k) => k.name === `${network}/abandon-art/pp0/acct0`);
    const z = await startZdev({ yes: true, network });
    if (!z) {
      check(`${tag}: zdev`, false, 'zdev could not be built');
      return;
    }
    let tab = null;
    const pages = [];
    try {
      await call(page, 'cancelSync');
      await call(page, 'wipe', { network });
      await call(page, 'open', { network, server: network === 'test' ? LWD : '' });
      const tip = await call(page, 'tip', {});
      const back = network === 'main' ? 100_000 : 200_000;
      const imp = await call(page, 'importAccount', { ufvk: v.ufvk, name: 'board', birthday: tip.height - back, origin: { seedFingerprint: v.seed_fingerprint, accountIndex: 0 } });
      const uuid = imp.account.uuid;
      await page.evaluate((n) => chrome.storage.local.set({ prefs: { network: n, servers: {}, account: {} } }), network);
      // A sync running, as right after an import.
      await page.evaluate(() => void window.t.call('sync', {}).catch(() => {}));

      tab = await popupWithBluetooth(ctx, id, z.link, { hangStopNotifications: true });
      await tab.waitForSelector('#ble-press', { timeout: 60e3 });
      await tab.click('#connect-ble');
      await tab.waitForSelector('#account', { timeout: 180e3 }).catch(async (e) => {
        console.log(`  tab shows: ${await text(tab)}`);
        throw e;
      });
      const shown = await tab.evaluate(() => ({ sync: document.getElementById('sync-line')?.textContent, balance: document.getElementById('balances')?.textContent }));
      check(`${tag}: the tab shows its board's new account while it syncs`, /^Syncing/.test(shown.sync ?? '') && /No balance yet: sync first/.test(shown.balance ?? ''), `"${shown.sync}", "${shown.balance}"`);

      // Another view, opened while the tab holds the board over Bluetooth.
      const other = await ctx.newPage();
      pages.push(other);
      await other.goto(`chrome-extension://${id}/popup.html`);
      await other.waitForSelector('#connect-to-see', { timeout: 60e3 });
      check(`${tag}: a popup opened meanwhile shows nothing of it (its board is in the tab)`, !(await other.evaluate(WALLET_SHOWN)));

      await tab.click('#disconnect');
      const hidMs = await within(tab, () => !!document.querySelector('#connect-to-see') && !['#account', '#total', '#ua', '#send', '#history', '#sync', '#balances'].some((s) => document.querySelector(s)), 2000);
      check(`${tag}: Disconnect hides the wallet at once, though the Bluetooth stack hangs`, hidMs !== null, hidMs === null ? `still shown: ${(await text(tab)).slice(0, 200)}` : `${hidMs} ms`);
      const dropped = await within(tab, () => window.gattDisconnects > 0, 3000);
      check(`${tag}: ... and the GATT connection is dropped`, dropped !== null);
      const hsNow = await call(page, 'hostState');
      check(`${tag}: the offscreen document holds no USB session`, hsNow.board?.phase === 'disconnected', hsNow.board?.phase);
      check(`${tag}: the popup opened meanwhile still shows nothing`, !(await other.evaluate(WALLET_SHOWN)));
      await other.close();
      const again = await ctx.newPage();
      pages.push(again);
      await again.goto(`chrome-extension://${id}/popup.html`);
      await again.waitForSelector('#connect-to-see', { timeout: 60e3 });
      check(`${tag}: the popup reopened shows nothing until its board connects`, !(await again.evaluate(WALLET_SHOWN)));

      // Forget, with the sync still running.
      await tab.click('#connect-ble');
      await tab.waitForSelector('#forget', { timeout: 180e3 });
      const syncing = (await call(page, 'hostState')).busy === 'sync';
      await tab.click('#forget');
      await tab.waitForSelector('#forget-confirm', { timeout: 10e3 });
      await tab.click('#forget-yes');
      const goneMs = await within(tab, () => !['#account', '#total', '#ua', '#send', '#history', '#sync', '#balances', '#forget'].some((s) => document.querySelector(s)), 2000);
      check(`${tag}: Forget hides the wallet at once${syncing ? ' while the sync runs' : ''}`, goneMs !== null, goneMs === null ? `still shown: ${(await text(tab)).slice(0, 200)}` : `${goneMs} ms${syncing ? '' : ' (the sync had already ended)'}`);
      const t0 = Date.now();
      await tab.waitForSelector('#import-board:not([disabled])', { timeout: 600e3 });
      const st = await call(page, 'status');
      check(`${tag}: ... and deletes it once the sync has stopped`, !st.accounts.some((a) => a.uuid === uuid) && !(await tab.evaluate(WALLET_SHOWN)), ms(t0));
    } catch (e) {
      check(`${tag}: board views, no exception`, false, String(e?.stack ?? e));
    } finally {
      for (const p of pages) await p.close().catch(() => {});
      await tab?.closeBridge();
      z.stop();
    }
  }
  await call(page, 'cancelSync');
  await page.evaluate(() => chrome.storage.local.set({ prefs: { network: 'test', servers: {}, account: {} } }));
}

const text = (popup) => popup.evaluate(() => document.body.innerText);
const has = (popup, sel) => popup.evaluate((s) => !!document.querySelector(s), sel);

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

  if (process.env.SKIP_TO !== '9') {
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
    const txStep = events.find((e) => e.phase === 'transactions');
    check('sync ends with the full-transaction step (transparent spends)', !!synced.transactions && !!txStep && synced.transactions.failed === 0, JSON.stringify(synced.transactions));

    // 4b. A transparent coin spent by a transaction this wallet did not see
    // (a shield made by another wallet with the same keys): compact blocks
    // carry no transparent inputs, so only the full-transaction step finds
    // the spend. Simulated by forgetting every recorded spend.
    {
      const tBefore = (await call(page, 'balances', { account: uuid })).balances.transparent;
      const { forgotten } = await call(page, 'devForgetTransparentSpends');
      if (!forgotten) {
        console.log('  note: no spent transparent coin in this wallet: the spend search is not exercised');
      } else {
        const tStale = (await call(page, 'balances', { account: uuid })).balances.transparent;
        const r = await call(page, 'sync', {});
        const tAfter = (await call(page, 'balances', { account: uuid })).balances.transparent;
        check('a forgotten transparent spend makes the balance stale', tStale.total > tBefore.total, `${forgotten} spend(s) forgotten: transparent ${tBefore.total} -> ${tStale.total} zat`);
        check('the next sync finds the spends again (UTXO set, t-address transactions)', tAfter.total === tBefore.total && r.transactions?.vanishedCoins > 0, `transparent ${tAfter.total} zat, ${JSON.stringify(r.transactions)}`);
      }
    }

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
    const lines = history.map(historyEntry);
    check('history lines: no send shows as -0', lines.every((l) => l.amount !== '-0.00000000 ZEC'), `${lines.filter((l) => l.label.startsWith('sent to yourself')).length} sent to yourself`);
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

    // 8. The popup. No board connected: the board section only, nothing of
    // the board's wallet.
    const popup = await ctx.newPage();
    popup.on('pageerror', (e) => console.log(`  [popup error] ${e.message}`));
    await popup.goto(`chrome-extension://${id}/popup.html?tab`);
    await popup.waitForSelector('#connect-to-see', { timeout: 120e3 }).catch(async (e) => {
      console.log(`  popup shows: ${await text(popup)}`);
      throw e;
    });
    let shownText = await text(popup);
    const leaks = await popup.evaluate(() => ['#total', '#ua', '#history', '#send', '#txid', '#sync', '#import'].filter((s) => document.querySelector(s)));
    check('without its board the popup shows nothing of the wallet', leaks.length === 0 && !shownText.includes(ua0) && !shownText.includes('ZEC'), leaks.join(' ') || 'board section only');
    const bt = await popup.evaluate(() => ({ native: !!navigator.bluetooth, disabled: document.getElementById('connect-ble')?.disabled, help: document.getElementById('ble-help')?.textContent ?? null }));
    check('the Bluetooth button stays usable; without Web Bluetooth the tab says how to enable it', bt.disabled === false && (bt.native || /flags/.test(bt.help ?? '')), bt.native ? 'this Chromium has navigator.bluetooth' : bt.help);
    // "Connect over USB" with no port granted (headless: none exists) asks
    // for the browser's chooser, which a tab can open.
    await popup.click('#connect-usb');
    const granted = await popup.waitForSelector('#grant-usb', { timeout: 30e3 }).then(() => true, () => false);
    check('USB with no port granted: the offscreen document asks for the chooser', granted);
    await popup.close();
    // From the toolbar popup (not a tab): with no port granted, a tab opens
    // for the chooser and says why; Bluetooth's tab is explained in a line.
    {
      const pop = await ctx.newPage();
      await pop.goto(`chrome-extension://${id}/popup.html`);
      await pop.waitForSelector('#connect-usb:not([disabled])', { timeout: 120e3 });
      const bleWhy = await pop.evaluate(() => document.getElementById('ble-why')?.textContent ?? '');
      const [usbTab] = await Promise.all([ctx.waitForEvent('page', { predicate: (p) => p.url().includes('connect=usb'), timeout: 30e3 }), pop.click('#connect-usb')]);
      const why = await usbTab.waitForSelector('#usb-why', { timeout: 60e3 }).then((e) => e.textContent(), () => null);
      const choose = await has(usbTab, '#grant-usb');
      check('the popup with no USB port granted opens a tab for the chooser, and the tab says why', !!why && /First USB connection/.test(why) && choose, why ?? (await text(usbTab)).slice(0, 200));
      check('the popup says in one line why Bluetooth needs a tab', /tab/.test(bleWhy), bleWhy);
      await usbTab.close();
      await pop.close().catch(() => {});
    }

    // Its board (zdev, abandon-art) over Bluetooth: the wallet appears.
    let zb = await startZdev({ yes: true });
    if (!zb) {
      check('popup with the board: zdev', false, 'zdev could not be built');
    } else {
      try {
        t0 = Date.now();
        let bp = await popupWithBluetooth(ctx, id, zb.link);
        await bp.waitForSelector('#ble-press', { timeout: 60e3 });
        await bp.click('#connect-ble');
        await bp.waitForSelector('#total', { timeout: 180e3 }).catch(async (e) => {
          console.log(`  popup shows: ${await text(bp)}`);
          throw e;
        });
        const ui = await bp.evaluate(() => ({ ua: document.getElementById('ua')?.textContent, board: document.getElementById('board-ready')?.textContent, send: !!document.getElementById('send'), verify: !!document.getElementById('verify'), forget: !!document.getElementById('forget') }));
        check('its board connected (Bluetooth): the popup shows the balance, address, send and verify', ui.ua === ua0 && ui.send && ui.verify && /Bluetooth/.test(ui.board), `${ui.board}, ${ms(t0)}`);
        check('"Forget this board" is offered to that board', ui.forget);
        await bp.click('#disconnect');
        await bp.waitForSelector('#connect-to-see', { timeout: 30e3 });
        shownText = await text(bp);
        check('Disconnect hides the wallet again', !(await has(bp, '#total')) && !shownText.includes(ua0) && !shownText.includes('ZEC'));
        await bp.closeBridge();

        // A passphrase typed in the popup (the weaker path): the board shows
        // and confirms the XFP; the hidden wallet is another account.
        const hk = keys.find((k) => k.name === 'test/abandon-art/pp1/acct0');
        let hiddenUuid = null;
        try {
          const pp = await popupWithBluetooth(ctx, id, zb.link);
          await pp.waitForSelector('#ble-press', { timeout: 60e3 });
          await pp.click('#connect-ble');
          await pp.waitForSelector('#total', { timeout: 180e3 });
          await pp.click('#passpanel > summary');
          await pp.click('#passdisclose > summary');
          await pp.fill('#passphrase', hk.passphrase);
          await pp.click('#passapply');
          await pp.waitForSelector('#import-board', { timeout: 120e3 });
          shownText = await text(pp);
          const hint = await pp.evaluate(() => document.getElementById('passhint')?.textContent ?? '');
          const field = await pp.evaluate(() => document.getElementById('passphrase')?.value);
          check(
            'a passphrase typed here: the board shows the XFP, the wallet without it is hidden, the hidden one is offered as its own import',
            /XFP [0-9A-F]{8}/.test(hint) && (await has(pp, '#hidden-import')) && !(await has(pp, '#total')) && !shownText.includes(ua0) && field === '' && !shownText.includes(hk.passphrase),
            hint,
          );
          await pp.click('#import-board');
          await pp.waitForSelector('#birthday-form', { timeout: 60e3 });
          await pp.click('#birthday-form button[type=submit]');
          await pp.waitForSelector('#ua', { timeout: 180e3 });
          const hst = await call(page, 'status');
          const ha = hst.accounts.find((a) => a.origin?.seedFingerprint === hk.seed_fingerprint);
          hiddenUuid = ha?.uuid ?? null;
          const hua = await pp.evaluate(() => document.getElementById('ua')?.textContent);
          check('the hidden wallet is a separate device account, keyed by its fingerprint', !!ha && ha.ufvk === hk.ufvk && hst.accounts.length === 2 && hua === hk.ua && hua !== ua0, `${ha?.name}, ZFP ${ha?.shortFingerprint}`);
          const cache = await page.evaluate(() => chrome.storage.local.get('cache-test').then((c) => c['cache-test'] ?? null));
          check("the hidden wallet's balance and addresses are not cached in storage", cache?.shown !== hiddenUuid && !JSON.stringify(cache ?? {}).includes(hk.ua));
          await pp.click('#disconnect');
          await pp.waitForSelector('#connect-to-see', { timeout: 30e3 });
          shownText = await text(pp);
          check('Disconnect hides the hidden wallet', !(await has(pp, '#total')) && !shownText.includes(hk.ua));
          await pp.click('#connect-ble');
          await pp.waitForSelector('#ua', { timeout: 180e3 });
          const back = await pp.evaluate(() => ({ ua: document.getElementById('ua')?.textContent, state: document.getElementById('pass-state')?.textContent }));
          check('connected again, the board dropped the passphrase: the wallet without it, never the hidden one', back.ua === ua0 && /No passphrase/.test(back.state ?? ''), back.state);
          await pp.closeBridge();
        } catch (e) {
          check('passphrase in the popup: no exception', false, String(e?.stack ?? e));
        } finally {
          await call(page, 'cancelSync').catch(() => {});
          if (hiddenUuid) await call(page, 'deleteAccount', { account: hiddenUuid }).catch(() => {});
        }

        // A board with another wallet: none of this one, and its own import.
        const zo = await startZdev({ yes: true, passphrase: 'leekwallet-e2e-other-board' });
        try {
          const op = await popupWithBluetooth(ctx, id, zo.link);
          await op.waitForSelector('#ble-press', { timeout: 60e3 });
          await op.click('#connect-ble');
          await op.waitForSelector('#import-board', { timeout: 120e3 });
          shownText = await text(op);
          const other = await op.evaluate(() => ['#total', '#ua', '#history', '#send', '#forget', '#connect-to-see'].filter((s) => document.querySelector(s)));
          check('a board with another wallet sees none of it and is offered its own import', other.length === 0 && !shownText.includes(ua0) && !shownText.includes('ZEC'), other.join(' ') || 'import only');
          await op.closeBridge();
        } finally {
          zo.stop();
        }
      } catch (e) {
        check('popup with the board: no exception', false, String(e?.stack ?? e));
      }
    }

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

    // "Forget this board": its account and synced data go; the board stays.
    if (zb) {
      try {
        const bp = await popupWithBluetooth(ctx, id, zb.link);
        await bp.waitForSelector('#ble-press', { timeout: 60e3 });
        await bp.click('#connect-ble');
        await bp.waitForSelector('#forget', { timeout: 180e3 });
        await bp.click('#forget');
        await bp.waitForSelector('#forget-confirm', { timeout: 10e3 });
        await bp.click('#forget-yes');
        await bp.waitForSelector('#import-board', { timeout: 180e3 });
        // Hidden at once; deleted once a sync running (the popup starts one
        // on open) has stopped.
        const tf = Date.now();
        for (;;) {
          st = await call(page, 'status');
          if (!st.accounts.length || Date.now() - tf > 120e3) break;
          await new Promise((r) => setTimeout(r, 250));
        }
        const gone = await call(page, 'balances', { account: uuid }).then(() => false, () => true);
        check('"Forget this board" (confirmed) deletes the account and its data', st.accounts.length === 0 && gone && !(await has(bp, '#total')), `${(st.dbBytes / 1e6).toFixed(2)} MB left (chain data)`);
        await bp.closeBridge();
      } catch (e) {
        check('forget: no exception', false, String(e?.stack ?? e));
      } finally {
        zb.stop();
      }
    }
    if (mem?.memoryMb) console.log(`  WASM memory ${mem.memoryMb.toFixed(0)} MB`);
  }

  // 9. The board's wallet in every view, while a sync runs.
  await boardViews(ctx, id, page);
} catch (e) {
  check('no exception', false, String(e?.stack ?? e));
} finally {
  await ctx.close().catch(() => {});
  if (!process.env.KEEP) rmSync(profile, { recursive: true, force: true });
}
console.log(`\n${results.length - failures}/${results.length} checks passed`);
process.exit(failures ? 1 : 0);
