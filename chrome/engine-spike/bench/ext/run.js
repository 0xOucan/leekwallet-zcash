// Loads a pkg, starts its thread pool if it has one, and runs the bench.
// Shared by the page (bench-page.js) and the dedicated worker (bench-worker.js).
import { runBench } from './bench.mjs';

export async function run(q, where, log) {
  const pkg = q.get('pkg') ?? 'pkg-prover';
  const env = {
    pkg,
    where,
    crossOriginIsolated: self.crossOriginIsolated,
    sharedArrayBuffer: typeof SharedArrayBuffer !== 'undefined',
    hardwareConcurrency: navigator.hardwareConcurrency,
  };
  try {
    log(JSON.stringify(env));
    const mod = await import(`./${pkg}/engine_spike.js`);
    await mod.default();
    if (mod.initThreadPool) {
      const n = Number(q.get('threads') ?? navigator.hardwareConcurrency);
      const t = performance.now();
      // A worker that cannot start (CSP) never answers: fail instead of hanging.
      await Promise.race([
        mod.initThreadPool(n),
        new Promise((_, reject) => setTimeout(() => reject(new Error('initThreadPool: no workers after 60 s')), 60e3)),
      ]);
      env.threads = n;
      env.thread_pool_ms = Math.round(performance.now() - t);
      log(`thread pool: ${n} workers in ${env.thread_pool_ms} ms`);
    }
    const pczt = new Uint8Array(await (await fetch('iw_send_shielded.pczt')).arrayBuffer());
    const out = await runBench(mod, {
      pczt,
      lwd: q.get('lwd'),
      scanCount: Number(q.get('scan') ?? 1000),
      sendZat: q.get('send'),
      onlyLwd: q.has('only_lwd'),
      log,
    });
    return { ...env, ...out };
  } catch (e) {
    log(`ERROR ${e?.stack ?? e}`);
    return { ...env, error: String(e?.stack ?? e) };
  }
}
