// The measurements, shared by Node (node.mjs) and Chromium (page.html).
// `mod` is a wasm-bindgen --target web module, already initialised.

// The public test mnemonic the vectors are generated with (vectors/pczt.json).
export const MNEMONIC =
  'abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon ' +
  'abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon art';

export async function runBench(mod, { pczt, lwd, scanStart, scanCount, onlyLwd, log }) {
  const out = {};
  const time = async (name, f) => {
    const t = performance.now();
    const r = await f();
    out[name] = Math.round(performance.now() - t);
    log(`${name}: ${out[name]} ms${r !== undefined ? ` (${typeof r === 'string' ? r : JSON.stringify(r)})` : ''}`);
    return r;
  };
  const prover = onlyLwd ? null : await time('proving_key_ms', () => new mod.Prover());
  if (prover) {
    await time('prove_2_actions_ms', () => prover.prove(1));
    await time('prove_2_actions_again_ms', () => prover.prove(1));
    await time('prove_3_actions_ms', () => prover.prove(3));
  }
  if (prover && mod.Verifier && pczt) {
    const verifier = await time('verifying_key_ms', () => new mod.Verifier());
    out.flow = JSON.parse(mod.sign_prove_extract(prover, verifier, pczt, MNEMONIC));
    log(`sign/prove/extract iw_send_shielded.pczt: ${JSON.stringify(out.flow)}`);
  }
  if (mod.lwd_info && lwd) {
    try {
      out.lwd = JSON.parse(await mod.lwd_info(lwd));
      log(`lwd_info ${lwd}: ${JSON.stringify(out.lwd)}`);
      const start = scanStart ?? out.lwd.tip - scanCount;
      out.scan = JSON.parse(await mod.fetch_and_scan(lwd, BigInt(start), BigInt(scanCount), MNEMONIC));
      log(`fetch_and_scan ${start}+${scanCount}: ${JSON.stringify(out.scan)}`);
    } catch (e) {
      out.lwd_error = String(e);
      log(`gRPC-web to ${lwd} failed: ${e}`);
    }
  }
  if (mod.wasm_memory_mb) out.wasm_memory_mb = Math.round(mod.wasm_memory_mb());
  if (globalThis.performance?.memory) {
    out.js_heap_mb = Math.round(performance.memory.usedJSHeapSize / 1e6);
  }
  return out;
}
