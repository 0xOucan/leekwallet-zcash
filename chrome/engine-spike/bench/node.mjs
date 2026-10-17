// Node as a proxy for the browser's single-threaded WASM:
//   node bench/node.mjs pkg-prover|pkg-engine [lightwalletd gRPC-web URL]
import { readFileSync } from 'node:fs';
import { resolve, dirname } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { runBench } from './bench.mjs';

const here = dirname(fileURLToPath(import.meta.url));
const pkg = resolve(here, '..', process.argv[2] ?? 'pkg-prover');
const lwd = process.argv[3];
const mod = await import(pathToFileURL(resolve(pkg, 'engine_spike.js')));
mod.initSync({ module: readFileSync(resolve(pkg, 'engine_spike_bg.wasm')) });
const pczt = readFileSync(resolve(here, '../../../vectors/pczt/iw_send_shielded.pczt'));

const out = await runBench(mod, { pczt, lwd, scanCount: 1000, log: console.log });
out.rss_mb = Math.round(process.memoryUsage().rss / 1e6);
console.log(JSON.stringify(out));
