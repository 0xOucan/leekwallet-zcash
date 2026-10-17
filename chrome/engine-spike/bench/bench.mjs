// The measurements, shared by Node (node.mjs) and Chromium (page.html).
// `mod` is a wasm-bindgen --target web module, already initialised.

// The public test mnemonic the vectors are generated with (vectors/pczt.json).
export const MNEMONIC =
  'abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon ' +
  'abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon art';

export async function runBench(mod, { pczt, lwd, scanStart, scanCount, sendZat, onlyLwd, log }) {
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
  if (mod.lwd_info && lwd && !mod.wallet_run) {
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
  if (mod.wallet_run && lwd) {
    // The whole companion flow: init + sync::run into zcash_client_sqlite,
    // propose + create the PCZT of a 1,000-zat send to self, then sign
    // (soft signer standing in for the board), prove, extract. No broadcast.
    try {
      const w = JSON.parse(await mod.wallet_run(lwd, MNEMONIC, scanCount, BigInt(sendZat ?? 1000)));
      const bytes = Uint8Array.from((w.pczt_hex.match(/../g) ?? []).map((h) => parseInt(h, 16)));
      delete w.pczt_hex;
      out.wallet = w;
      log(`wallet_run: ${JSON.stringify(w)}`);
      if (!bytes.length) throw new Error(`no PCZT: ${w.propose_error}`);
      const p = prover ?? (await time('proving_key_ms', () => new mod.Prover()));
      const v = await time('verifying_key_ms_wallet', () => new mod.Verifier());
      out.wallet_flow = JSON.parse(mod.sign_prove_extract(p, v, bytes, MNEMONIC));
      log(`sign/prove/extract of the fresh PCZT: ${JSON.stringify(out.wallet_flow)}`);
    } catch (e) {
      out.wallet_error = String(e?.stack ?? e);
      log(`wallet_run failed: ${out.wallet_error}`);
    }
  }
  if (mod.wasm_memory_mb) out.wasm_memory_mb = Math.round(mod.wasm_memory_mb());
  if (globalThis.performance?.memory) {
    out.js_heap_mb = Math.round(performance.memory.usedJSHeapSize / 1e6);
  }
  return out;
}
