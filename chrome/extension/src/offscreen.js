// The offscreen document: starts the engine worker and serves it to the
// extension's pages over chrome.runtime ports.
//
// Why here: the service worker is stopped when idle, and a sync or a proof
// outlives it; a page (the popup) closes whenever it loses focus. This
// document stays until the browser closes, so the engine keeps syncing
// with the popup shut, and the popup finds it mid-sync when it reopens.
// In 9.3 the board link (Web Serial, Web Bluetooth) lives here too.
//
// Port protocol (name "engine"), JSON both ways:
//   page -> here: { id, method, params }
//   here -> page: { id, result } | { id, error }   the answer to that page
//                 { event }                         every progress event, to every page
//                 { host }                          engine state changes, to every page
// `hostState` is answered here: whether the engine is up, what it is doing,
// and the last sync progress, so a popup opened mid-sync can show it.

const ports = new Set();
let worker = null;
let seq = 0;
const inflight = new Map(); // worker id -> { port, id, method }
const host = {
  ready: false,
  info: null,
  fatal: null,
  restarts: 0,
  busy: null, // method running in the engine
  network: null, // the wallet open in the engine
  server: null,
  lastSync: null, // the last sync event
  lastSyncResult: null,
};

function broadcast(msg) {
  for (const p of ports) {
    try {
      p.postMessage(msg);
    } catch {
      ports.delete(p);
    }
  }
}

function hostChanged() {
  broadcast({ host: { ...host } });
}

function startWorker() {
  host.ready = false;
  host.fatal = null;
  worker = new Worker(new URL('./engine-worker.js', import.meta.url), { type: 'module' });
  worker.onmessage = ({ data }) => onWorkerMessage(data);
  worker.onerror = (e) => {
    console.error('engine worker error', e.message);
    onFatal(`worker error: ${e.message}`);
  };
}

function onFatal(why) {
  host.fatal = why;
  host.ready = false;
  for (const [, req] of inflight) reply(req, { error: `engine stopped: ${why}` });
  inflight.clear();
  hostChanged();
  worker?.terminate();
  worker = null;
  // A trap leaves the WASM instance unusable: start a fresh one, reopening
  // the same wallet. Give up after a few, rather than loop on a crash.
  if (host.restarts < 3) {
    host.restarts += 1;
    setTimeout(async () => {
      startWorker();
      if (host.network) await callEngine('open', { network: host.network, server: host.server }).catch(() => {});
    }, 1000);
  }
}

function reply({ port, id }, msg) {
  if (!port) return;
  try {
    port.postMessage({ id, ...msg });
  } catch {
    ports.delete(port);
  }
}

function onWorkerMessage(data) {
  if (data.ready) {
    host.ready = true;
    host.info = data.info;
    hostChanged();
    return;
  }
  if (data.fatal) {
    onFatal(data.fatal);
    return;
  }
  if (data.event) {
    if (data.event.event === 'sync') host.lastSync = data.event;
    broadcast({ event: data.event });
    return;
  }
  const req = inflight.get(data.id);
  if (!req) return;
  inflight.delete(data.id);
  if (req.method === host.busy) host.busy = null;
  if ('error' in data) {
    reply(req, { error: data.error });
  } else {
    if (req.method === 'open') {
      host.network = data.result.network;
      host.server = data.result.server;
    }
    if (req.method === 'setServer') host.server = data.result.server;
    if (req.method === 'sync') host.lastSyncResult = { network: host.network, ...data.result };
    reply(req, { result: data.result, ms: data.ms });
  }
  if (req.method === 'open' || req.method === 'sync' || req.method === 'wipe') hostChanged();
}

/** Sends a call to the engine; `port` (may be null) gets the answer. */
function send(port, id, method, params) {
  if (!worker) {
    reply({ port, id }, { error: `engine is not running: ${host.fatal ?? 'starting'}` });
    return;
  }
  const wid = ++seq;
  inflight.set(wid, { port, id, method });
  if (method === 'sync') {
    host.busy = 'sync';
    host.lastSync = null;
    hostChanged();
  }
  worker.postMessage({ id: wid, method, params });
}

/** The same, as a promise, for this document's own calls. */
function callEngine(method, params) {
  return new Promise((resolve, reject) => {
    const fake = {
      postMessage: (m) => ('error' in m ? reject(new Error(m.error)) : resolve(m.result)),
    };
    send(fake, 0, method, params);
  });
}

chrome.runtime.onConnect.addListener((port) => {
  if (port.name !== 'engine') return;
  ports.add(port);
  port.onDisconnect.addListener(() => {
    ports.delete(port);
    // The answer to a closed popup has nowhere to go; the call still runs.
    for (const req of inflight.values()) if (req.port === port) req.port = null;
  });
  port.onMessage.addListener(({ id, method, params }) => {
    if (method === 'hostState') {
      reply({ port, id }, { result: { ...host } });
      return;
    }
    send(port, id, method, params);
  });
  port.postMessage({ host: { ...host } });
});

startWorker();
