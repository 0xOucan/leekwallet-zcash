// bench.html?pkg=pkg-prover|pkg-engine|pkg-threads...&threads=N&lwd=URL&where=tab|offscreen|worker
//
// `worker` runs the bench in a dedicated worker started by this page (an
// offscreen document, from chromium.mjs). Only a worker may block, and
// zcash_client_backend's scanner blocks on its rayon results (see README).
import { run } from './run.js';

const q = new URLSearchParams(location.search);
const where = q.get('where') ?? 'tab';
const logEl = document.getElementById('log');
const log = (s) => {
  logEl.textContent += s + '\n';
  console.log(s);
  if (where !== 'tab') chrome.runtime.sendMessage({ log: s });
};
const report = (bench) => chrome.runtime.sendMessage({ bench });

if (where === 'worker') {
  const w = new Worker('bench-worker.js', { type: 'module' });
  w.onmessage = ({ data }) => (data.log !== undefined ? log(data.log) : report(data.result));
  w.onerror = (e) => report({ pkg: q.get('pkg'), where, error: `worker: ${e.message}` });
  w.postMessage(location.search);
} else {
  report(await run(q, where, log));
}
