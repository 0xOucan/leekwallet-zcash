// Loads bench/ext as an unpacked MV3 extension in Playwright's Chromium and
// runs the bench in an extension tab or in an offscreen document.
//
//   node chromium.mjs <where>:<pkg>[:<threads>] ...   e.g.
//   node chromium.mjs tab:pkg-prover offscreen:pkg-threads:4 offscreen:pkg-engine
//
// Env: LWD (gRPC-web URL, default https://zcash-testnet.chainsafe.dev),
//      SCAN (blocks to fetch and scan, default 1000), ONLY_LWD=1 (skip proving), HEADED=1.
import { chromium } from 'playwright-core';
import { cpSync, existsSync, mkdirSync, rmSync, mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const root = resolve(here, '..');
const ext = join(root, 'target', 'ext');
rmSync(ext, { recursive: true, force: true });
mkdirSync(ext, { recursive: true });
cpSync(join(here, 'ext'), ext, { recursive: true });
cpSync(join(here, 'bench.mjs'), join(ext, 'bench.mjs'));
cpSync(join(root, '../../vectors/pczt/iw_send_shielded.pczt'), join(ext, 'iw_send_shielded.pczt'));
for (const p of ['pkg-prover', 'pkg-engine', 'pkg-threads', 'pkg-threads-engine']) {
  if (existsSync(join(root, p))) cpSync(join(root, p), join(ext, p), { recursive: true });
}

const lwd = process.env.LWD ?? 'https://zcash-testnet.chainsafe.dev';
const scan = process.env.SCAN ?? '1000';
const ctx = await chromium.launchPersistentContext(mkdtempSync(join(tmpdir(), 'spike-')), {
  channel: 'chromium', // full Chromium in new headless mode: extensions load
  headless: !process.env.HEADED,
  args: [
    `--disable-extensions-except=${ext}`,
    `--load-extension=${ext}`,
    // Chromium 137+ ignores --load-extension unless this is turned off.
    '--disable-features=DisableLoadExtensionCommandLineSwitch',
  ],
});
let [sw] = ctx.serviceWorkers();
if (!sw) sw = await ctx.waitForEvent('serviceworker');
const id = new URL(sw.url()).host;
console.log(`extension ${id}, ${await ctx.browser()?.version?.() ?? 'chromium'}`);

// `rawgrpc`: plain gRPC (application/grpc, no proxy) to testnet.zec.rocks
// with fetch() from the extension's service worker. host_permissions lift
// CORS; the question is what fetch can read of an HTTP/2 gRPC response.
async function rawGrpc() {
  return sw.evaluate(async () => {
    const call = async (method, body) => {
      const r = await fetch(`https://testnet.zec.rocks/cash.z.wallet.sdk.rpc.CompactTxStreamer/${method}`, {
        method: 'POST',
        headers: { 'content-type': 'application/grpc+proto', 'grpc-accept-encoding': 'identity' },
        body: new Uint8Array([0, ...new Uint8Array(new Uint32Array([body.length]).buffer).reverse(), ...body]),
      });
      const bytes = new Uint8Array(await r.arrayBuffer());
      const headers = Object.fromEntries(r.headers.entries());
      const len = bytes.length >= 5 ? new DataView(bytes.buffer).getUint32(1) : -1;
      return { status: r.status, headers, bytes: bytes.length, first_message_len: len };
    };
    const out = {};
    try { out.GetLightdInfo = await call('GetLightdInfo', []); } catch (e) { out.GetLightdInfo = String(e); }
    // ChainSpec {} is an empty message too.
    try { out.GetLatestBlock = await call('GetLatestBlock', []); } catch (e) { out.GetLatestBlock = String(e); }
    // A bad request: the error status travels in trailers only after data, or in headers.
    try { out.BadMethod = await call('NoSuchMethod', []); } catch (e) { out.BadMethod = String(e); }
    return out;
  });
}

for (const spec of process.argv.slice(2)) {
  if (spec === 'rawgrpc') {
    console.log(`RESULT rawgrpc ${JSON.stringify(await rawGrpc())}`);
    continue;
  }
  const [where, pkg, threads] = spec.split(':');
  const query = new URLSearchParams({
    pkg, lwd, scan,
    ...(threads ? { threads } : {}),
    ...(process.env.ONLY_LWD ? { only_lwd: '1' } : {}),
  }).toString();
  const before = await sw.evaluate(() => globalThis.results.length);
  const t = Date.now();
  let page;
  if (where === 'offscreen') {
    await sw.evaluate((q) => globalThis.runOffscreen(q), query);
  } else {
    page = await ctx.newPage();
    page.on('console', (m) => console.log(`  [${pkg}] ${m.text()}`));
    await page.goto(`chrome-extension://${id}/bench.html?${query}&where=tab`);
  }
  let result;
  while (!(result = await sw.evaluate((n) => globalThis.results[n], before))) {
    if (Date.now() - t > 30 * 60e3) throw new Error(`${spec}: timed out`);
    await new Promise((r) => setTimeout(r, 2000));
  }
  console.log(`RESULT ${spec} ${JSON.stringify(result)}`);
  await page?.close();
}
await ctx.close();
