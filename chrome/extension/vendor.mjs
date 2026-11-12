// Copies the third-party modules the extension's pages import into
// src/vendor/ (not in git), from the versions package-lock.json pins, as
// plain ES modules with only relative imports, so no bundler is needed.
// build.mjs and the tests run it.
//
//   @noble/ciphers   ChaCha20-Poly1305 for the board's session
//   qrcode-generator the QR codes shown to the board (Sign with QR), the
//                    desktop app's encoder
//   zxing-wasm       the webcam's QR reader (its reader build and .wasm),
//                    the desktop app's decoder. Its CDN default for the
//                    .wasm is replaced by the packaged file: nothing is
//                    fetched from outside the extension.
//
//   node vendor.mjs
import { cp, mkdir, readFile, stat, writeFile } from 'node:fs/promises';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const mods = join(here, 'node_modules');
const out = join(here, 'src/vendor');

const exists = (p) => stat(p).then(() => true, () => false);

async function pkg(name, probe) {
  const dir = join(mods, name);
  if (!(await exists(join(dir, probe)))) throw new Error(`no node_modules/${name}: run npm install in chrome/extension`);
  const { version } = JSON.parse(await readFile(join(dir, 'package.json'), 'utf8'));
  return { dir, version };
}

export async function vendor() {
  const noble = await pkg('@noble/ciphers', 'esm/chacha.js');
  await mkdir(join(out, 'noble-ciphers'), { recursive: true });
  for (const f of ['chacha.js', '_arx.js', '_poly1305.js', 'utils.js']) await cp(join(noble.dir, 'esm', f), join(out, 'noble-ciphers', f));
  await cp(join(noble.dir, 'LICENSE'), join(out, 'noble-ciphers/LICENSE'));

  // MIT; the licence is in the file's header (the package ships no LICENSE).
  const qr = await pkg('qrcode-generator', 'dist/qrcode.mjs');
  await mkdir(join(out, 'qrcode-generator'), { recursive: true });
  await cp(join(qr.dir, 'dist/qrcode.mjs'), join(out, 'qrcode-generator/qrcode.mjs'));

  const zx = await pkg('zxing-wasm', 'dist/es/reader/index.js');
  const zo = join(out, 'zxing-wasm');
  await mkdir(join(zo, 'es/reader'), { recursive: true });
  await mkdir(join(zo, 'reader'), { recursive: true });
  const index = await readFile(join(zx.dir, 'dist/es/reader/index.js'), 'utf8');
  const imports = [...index.matchAll(/from\s+"([^"]+)"/g)].map((m) => m[1]);
  if (imports.some((i) => i !== '../share.js')) throw new Error(`zxing-wasm's reader imports ${imports.join(', ')}: update vendor.mjs`);
  await writeFile(join(zo, 'es/reader/index.js'), index);
  const share = await readFile(join(zx.dir, 'dist/es/share.js'), 'utf8');
  const cdn = /`https:\/\/fastly\.jsdelivr\.net\/npm\/zxing-wasm@[^`]+`/;
  if (!cdn.test(share)) throw new Error("zxing-wasm's share.js changed its CDN default: update vendor.mjs");
  // The default locateFile, should anything call it: the packaged file.
  await writeFile(join(zo, 'es/share.js'), share.replace(cdn, 'new URL(`../${n[1]}/${e}`, import.meta.url).href'));
  await cp(join(zx.dir, 'dist/reader/zxing_reader.wasm'), join(zo, 'reader/zxing_reader.wasm'));
  await cp(join(zx.dir, 'LICENSE'), join(zo, 'LICENSE'));

  return `@noble/ciphers ${noble.version}, qrcode-generator ${qr.version}, zxing-wasm ${zx.version}`;
}

if (process.argv[1] === fileURLToPath(import.meta.url)) {
  console.log(`vendored ${await vendor()} into src/vendor/`);
}
