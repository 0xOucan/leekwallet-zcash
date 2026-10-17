// Assembles the unpacked extension in dist/. No bundler: the pages and the
// worker are plain ES modules, loaded as written (readable is auditable),
// and the engine is wasm-bindgen's `--target web` output, whose thread
// workers import their own module by relative URL.
//
//   node build.mjs            needs ../engine/pkg (../engine/build.sh)
//
// dist/ mirrors this directory, plus:
//   engine/      ../engine/pkg (the WASM engine and its JS glue)
//   tokens.css   firmware/app/src/tokens.css (LeekWallet's design tokens)
//   icons/       firmware/app/extension/icons
import { cp, mkdir, readFile, rm, stat } from 'node:fs/promises';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const repo = join(here, '..', '..');
const dist = join(here, 'dist');
const pkg = process.env.ENGINE_PKG ?? join(here, '..', 'engine', 'pkg');

const exists = (p) => stat(p).then(() => true, () => false);

if (!(await exists(join(pkg, 'leek_chrome_engine_bg.wasm')))) {
  console.error(`no engine in ${pkg}: run chrome/engine/build.sh first`);
  process.exit(1);
}

await rm(dist, { recursive: true, force: true });
await mkdir(dist, { recursive: true });
for (const f of ['manifest.json', 'popup.html', 'popup.css', 'offscreen.html']) {
  await cp(join(here, f), join(dist, f));
}
await cp(join(here, 'src'), join(dist, 'src'), { recursive: true });
await cp(pkg, join(dist, 'engine'), {
  recursive: true,
  // wasm-bindgen's package.json and .d.ts files are not needed at runtime.
  filter: (p) => !/\.d\.ts$|package\.json$|\.gitignore$/.test(p),
});
await cp(join(repo, 'firmware/app/src/tokens.css'), join(dist, 'tokens.css'));
await cp(join(repo, 'firmware/app/extension/icons'), join(dist, 'icons'), { recursive: true });

// The manifest is copied, never generated: it is the file that declares the
// permissions, so it is reviewed as written. Check it names files we built.
const manifest = JSON.parse(await readFile(join(dist, 'manifest.json'), 'utf8'));
const required = [
  manifest.background.service_worker,
  manifest.action.default_popup,
  'offscreen.html',
  'src/offscreen.js',
  'src/engine-worker.js',
  'engine/leek_chrome_engine.js',
  'engine/leek_chrome_engine_bg.wasm',
  ...Object.values(manifest.icons),
];
const missing = [];
for (const f of required) if (!(await exists(join(dist, f)))) missing.push(f);
if (missing.length) {
  console.error(`the build is missing files the extension needs: ${missing.join(', ')}`);
  process.exit(1);
}
if (manifest.cross_origin_embedder_policy?.value !== 'require-corp' || manifest.cross_origin_opener_policy?.value !== 'same-origin') {
  console.error('the manifest lost COOP/COEP: without cross-origin isolation the engine has no threads and cannot sync');
  process.exit(1);
}
const wasm = await stat(join(dist, 'engine/leek_chrome_engine_bg.wasm'));
console.log(`LeekWallet Zcash ${manifest.version} in ${dist} (engine ${(wasm.size / 1e6).toFixed(1)} MB). Load it unpacked from chrome://extensions.`);
