// The dedicated worker that holds the WASM engine (chrome/engine).
//
// A worker, not the offscreen page: the threads build blocks in
// Atomics.wait while rayon decrypts and proves, which a page may not do,
// and OPFS sync access handles (the wallet database) exist only in
// dedicated workers. See docs/zcash/CHROME-ENGINE.md.
//
// Protocol (structured-clone JSON both ways):
//   in:  { id, method, params }
//   out: { id, result } | { id, error }      one per request
//        { event }                            progress, any time
//        { ready, info } | { fatal }          once, at start
//
// Calls run one at a time, in order: the engine borrows the wallet for a
// whole call, a sync included. `cancelSync` is the exception: it is
// answered at once and makes a running sync stop after its batch.

import init, { Engine, initStorage, initThreadPool, cancelSync, wasmMemoryMb } from '../engine/leek_chrome_engine.js';

const post = (m) => self.postMessage(m);
let engine = null;
let queue = Promise.resolve();
let busy = null; // the method running now
let dead = null; // set once the WASM traps (panic = abort): nothing more runs

async function start() {
  const t0 = performance.now();
  if (!self.crossOriginIsolated) {
    throw new Error('the engine needs cross-origin isolation (COOP/COEP) for its threads');
  }
  await init();
  // Leave a core for the browser; more than 8 threads gained little (9.1).
  const threads = Math.max(1, Math.min(8, (navigator.hardwareConcurrency || 2) - 1));
  await Promise.race([
    initThreadPool(threads),
    new Promise((_, reject) => setTimeout(() => reject(new Error('initThreadPool: no workers after 60 s')), 60e3)),
  ]);
  await initStorage();
  engine = new Engine();
  const info = JSON.parse(await engine.call('engineInfo', '{}', () => {}));
  return { ...info, threadsInPool: threads, startMs: Math.round(performance.now() - t0) };
}

const started = start().then(
  (info) => {
    post({ ready: true, info });
    return true;
  },
  (e) => {
    dead = String(e?.message ?? e);
    post({ fatal: dead });
    return false;
  },
);

async function run(id, method, params) {
  if (!(await started) || dead) {
    post({ id, error: `engine unavailable: ${dead}` });
    return;
  }
  busy = method;
  const t0 = performance.now();
  try {
    const out = await engine.call(method, JSON.stringify(params ?? {}), (ev) => post({ event: JSON.parse(ev) }));
    post({ id, result: JSON.parse(out), ms: Math.round(performance.now() - t0) });
  } catch (e) {
    // A RuntimeError is a WASM trap (a panic, with panic = "abort"): the
    // instance is unusable from here on. The offscreen document restarts
    // the worker when it sees `fatal`.
    if (e instanceof WebAssembly.RuntimeError) {
      dead = `engine crashed in ${method}: ${e.message}`;
      post({ id, error: dead });
      post({ fatal: dead });
    } else {
      post({ id, error: String(e?.message ?? e) });
    }
  } finally {
    busy = null;
  }
}

self.onmessage = ({ data }) => {
  const { id, method, params } = data ?? {};
  if (method === 'cancelSync') {
    if (engine) cancelSync();
    post({ id, result: { cancelling: busy === 'sync' } });
    return;
  }
  if (method === 'workerState') {
    post({ id, result: { busy, dead, memoryMb: engine ? wasmMemoryMb() : 0 } });
    return;
  }
  queue = queue.then(() => run(id, method, params));
};
