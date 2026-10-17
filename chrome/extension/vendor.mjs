// Copies the third-party modules the extension's pages import into
// src/vendor/ (not in git): @noble/ciphers' ChaCha20-Poly1305, from the
// version package-lock.json pins, as plain ES modules with only relative
// imports, so no bundler is needed. build.mjs and the tests run it.
//
//   node vendor.mjs
import { cp, mkdir, readFile, stat } from 'node:fs/promises';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));

export async function vendor() {
  const noble = join(here, 'node_modules/@noble/ciphers');
  const ok = await stat(join(noble, 'esm/chacha.js')).then(() => true, () => false);
  if (!ok) throw new Error('no node_modules/@noble/ciphers: run npm install in chrome/extension');
  const { version } = JSON.parse(await readFile(join(noble, 'package.json'), 'utf8'));
  const out = join(here, 'src/vendor/noble-ciphers');
  await mkdir(out, { recursive: true });
  for (const f of ['chacha.js', '_arx.js', '_poly1305.js', 'utils.js']) await cp(join(noble, 'esm', f), join(out, f));
  await cp(join(noble, 'LICENSE'), join(out, 'LICENSE'));
  return version;
}

if (process.argv[1] === fileURLToPath(import.meta.url)) {
  console.log(`vendored @noble/ciphers ${await vendor()} into src/vendor/`);
}
