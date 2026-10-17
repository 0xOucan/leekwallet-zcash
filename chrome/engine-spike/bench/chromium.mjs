// Loads bench/ext as an unpacked MV3 extension in Playwright's Chromium and
// runs the bench in an extension tab or in an offscreen document.
//
//   node chromium.mjs <where>:<pkg>[:<threads>] ...   e.g.
//   node chromium.mjs tab:pkg-prover offscreen:pkg-threads:4 offscreen:pkg-engine
//
// Env: LWD (gRPC-web URL, default https://zcash-testnet.chainsafe.dev),
//      SCAN (blocks to fetch and scan, default 1000), HEADED=1.
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

for (const spec of process.argv.slice(2)) {
  const [where, pkg, threads] = spec.split(':');
  const query = new URLSearchParams({ pkg, lwd, scan, ...(threads ? { threads } : {}) }).toString();
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
