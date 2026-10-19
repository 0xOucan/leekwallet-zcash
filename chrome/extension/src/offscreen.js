// The offscreen document: starts the engine worker and serves it to the
// extension's pages over chrome.runtime ports.
//
// Why here: the service worker is stopped when idle, and a sync or a proof
// outlives it; a page (the popup) closes whenever it loses focus. This
// document stays until the browser closes, so the engine keeps syncing
// with the popup shut, and the popup finds it mid-sync when it reopens.
// The board over USB lives here too (src/board/): Web Serial needs a
// document, and a signature must outlive the popup. Bluetooth cannot: the
// BluetoothDevice stays in the tab that chose it (src/board/ble-link.js).
//
// Port protocol (name "engine"), JSON both ways:
//   page -> here: { id, method, params }
//   here -> page: { id, result } | { id, error }   the answer to that page
//                 { event }                         every progress event, to every page
//                 { host }                          engine state changes, to every page
// `hostState` is answered here: whether the engine is up, what it is doing,
// and the last sync progress, so a popup opened mid-sync can show it.
// `board.<op>` calls go to the USB board (BoardHost.run), answered with
// `{ id, result }` or `{ id, error, errorData }`, and its state goes to every
// page as `{ board }`. `board.disconnect` also closes any serial port this
// document still holds; a page calls it before a Bluetooth connect and on
// a network switch. With no extension page left and nothing running on the
// board, the USB session ends after IDLE_MS: the port is not held idle.

import { BoardHost, BoardNeeds } from './board/host.js';
import { SerialLink, isBoardPort } from './board/serial-link.js';

const ports = new Set();
/** No page open and the board idle this long: the USB session ends. */
const IDLE_MS = 60e3;
let idleTimer = null;
let worker = null;
let seq = 0;
const inflight = new Map(); // worker id -> { port, id, method, params }
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
  // The engine's last `status` (accounts included), kept up to date with
  // imports and deletions: a page opened while a sync holds the engine
  // (its own `status` call waits behind it) still knows which accounts
  // there are, and so whether its board's wallet is among them.
  status: null,
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
    trackStatus(req.method, req.params, data.result);
    if (req.method === 'sync') host.lastSyncResult = { network: host.network, ...data.result };
    reply(req, { result: data.result, ms: data.ms });
  }
  if (['open', 'sync', 'wipe', 'status', 'importAccount', 'deleteAccount'].includes(req.method)) hostChanged();
}

function trackStatus(method, params, result) {
  if (method === 'open' || method === 'status') host.status = result;
  else if (method === 'wipe') host.status = null;
  else if (!host.status) return;
  else if (method === 'importAccount' && result.account && !host.status.accounts?.some((a) => a.uuid === result.account.uuid)) {
    host.status = { ...host.status, accounts: [...(host.status.accounts ?? []), result.account] };
  } else if (method === 'deleteAccount') {
    host.status = { ...host.status, accounts: (host.status.accounts ?? []).filter((a) => a.uuid !== params?.account) };
  }
}

/** Sends a call to the engine; `port` (may be null) gets the answer. */
function send(port, id, method, params) {
  if (!worker) {
    reply({ port, id }, { error: `engine is not running: ${host.fatal ?? 'starting'}` });
    return;
  }
  const wid = ++seq;
  inflight.set(wid, { port, id, method, params });
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

// ------------------------------------------------------------ the board

const board = new BoardHost({
  engine: callEngine,
  onState: (state) => broadcast({ board: state }),
  async openLink(kind, onClose) {
    if (kind !== 'usb') throw new Error('Bluetooth runs in an extension tab, not here');
    if (!navigator.serial) throw new Error('This browser has no Web Serial: use desktop Chrome, Edge or Brave.');
    // A link left open (it should not be) would make the port look taken.
    await SerialLink.closeAll();
    // Ports the user granted from a tab (requestPort needs a gesture).
    const ports = (await navigator.serial.getPorts()).filter(isBoardPort);
    if (!ports.length) throw new BoardNeeds('usbPermission', 'Choose the board once in the browser\'s USB chooser.');
    let lastError = null;
    for (const port of ports) {
      try {
        return await SerialLink.open(port, onClose);
      } catch (e) {
        lastError = e; // another program has it open, or it was unplugged
      }
    }
    throw new Error(`cannot open the board's serial port: ${lastError?.message ?? lastError} (is another program, zecz or the desktop app, using it?)`);
  },
});

async function runBoard(op, params) {
  if (op !== 'disconnect') return board.run(op, params);
  const r = await board.run('disconnect');
  await SerialLink.closeAll();
  return { ...r, portsOpen: SerialLink.openCount };
}

/** The last page closed: end an idle USB session after IDLE_MS; a page coming back keeps it. */
function watchIdle() {
  clearTimeout(idleTimer);
  idleTimer = null;
  if (ports.size) return;
  idleTimer = setTimeout(() => {
    idleTimer = null;
    if (ports.size) return;
    if (board.busy) return watchIdle(); // a signature outlives the popup
    if (board.state.phase !== 'disconnected' || SerialLink.openCount) void runBoard('disconnect').catch(() => {});
  }, IDLE_MS);
}

function boardCall(port, id, op, params) {
  runBoard(op, params ?? {}).then(
    (result) => reply({ port, id }, { result }),
    (e) => {
      const errorData = e instanceof BoardNeeds ? { need: e.need, tip: e.tip ?? null, account: e.account ?? null } : undefined;
      reply({ port, id }, { error: String(e?.message ?? e), errorData });
    },
  );
}

chrome.runtime.onConnect.addListener((port) => {
  if (port.name !== 'engine') return;
  ports.add(port);
  watchIdle();
  port.onDisconnect.addListener(() => {
    ports.delete(port);
    watchIdle();
    // The answer to a closed popup has nowhere to go; the call still runs.
    for (const req of inflight.values()) if (req.port === port) req.port = null;
  });
  port.onMessage.addListener(({ id, method, params }) => {
    if (method === 'hostState') {
      reply({ port, id }, { result: { ...host, board: { ...board.state } } });
      return;
    }
    if (typeof method === 'string' && method.startsWith('board.')) {
      boardCall(port, id, method.slice(6), params);
      return;
    }
    send(port, id, method, params);
  });
  port.postMessage({ host: { ...host } });
  port.postMessage({ board: { ...board.state } });
});

startWorker();
