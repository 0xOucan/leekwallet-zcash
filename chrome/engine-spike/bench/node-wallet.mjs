// The sqlite wallet flow (init, sync::run, propose, create PCZT) in Node,
// single-threaded:
//   node bench/node-wallet.mjs [blocks back, default 2000] [gRPC-web URL]
//
// It shows the single-threaded build's dead end: sync::run spins forever on
// the first block with shielded outputs, because zcash_client_backend hands
// trial decryption to rayon::spawn_fifo and blocks on the result, and with
// no threads nothing ever runs the job. The flow works in
// pkg-threads-sqlite inside a worker (chromium.mjs worker:pkg-threads-sqlite:4).
import { readFileSync } from 'node:fs';
import { resolve, dirname } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { MNEMONIC } from './bench.mjs';

const here = dirname(fileURLToPath(import.meta.url));
const pkg = resolve(here, '..', 'pkg-sqlite');
const mod = await import(pathToFileURL(resolve(pkg, 'engine_spike.js')));
mod.initSync({ module: readFileSync(resolve(pkg, 'engine_spike_bg.wasm')) });
const back = Number(process.argv[2] ?? 2000);
const lwd = process.argv[3] ?? 'https://zcash-testnet.chainsafe.dev';
try {
  const w = JSON.parse(await mod.wallet_run(lwd, MNEMONIC, back, 1000n));
  delete w.pczt_hex;
  console.log(JSON.stringify(w));
} catch (e) {
  console.log('ERROR', e);
}
console.log(`wasm memory ${Math.round(mod.wasm_memory_mb())} MB, rss ${Math.round(process.memoryUsage().rss / 1e6)} MB`);
