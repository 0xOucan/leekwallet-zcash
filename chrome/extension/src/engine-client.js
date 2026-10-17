// The page side of the engine: a port to the offscreen document.
//
//   const engine = await EngineClient.connect();
//   const status = await engine.call('open', { network: 'test' });
//   engine.onEvent((ev) => ...);         // sync / prove / import progress
//   engine.onHost((h) => ...);           // engine up/down, busy, last sync
//   engine.onBoard((b) => ...);          // the USB board (offscreen), `board.*` calls
//
// Every method of the engine's message API (chrome/README.md) goes through
// `call`. Errors reject with an Error carrying the engine's message.

export class EngineClient {
  #port;
  #seq = 0;
  #pending = new Map();
  #eventListeners = new Set();
  #hostListeners = new Set();
  #boardListeners = new Set();
  host = null;
  /** The USB board's state, from the offscreen document (src/board/host.js). */
  board = null;

  static async connect() {
    // The service worker creates the offscreen document if it is not there
    // (the first open after install, or after the browser restarted).
    const r = await chrome.runtime.sendMessage({ to: 'background', type: 'ensureEngine' });
    if (!r?.ok) throw new Error(`cannot start the engine: ${r?.error ?? 'no answer'}`);
    const c = new EngineClient();
    c.#open();
    return c;
  }

  #open() {
    this.#port = chrome.runtime.connect({ name: 'engine' });
    this.#port.onMessage.addListener((m) => {
      if (m.host) {
        this.host = m.host;
        for (const f of this.#hostListeners) f(m.host);
        return;
      }
      if (m.board) {
        this.board = m.board;
        for (const f of this.#boardListeners) f(m.board);
        return;
      }
      if (m.event) {
        for (const f of this.#eventListeners) f(m.event);
        return;
      }
      const p = this.#pending.get(m.id);
      if (!p) return;
      this.#pending.delete(m.id);
      if ('error' in m) p.reject(Object.assign(new Error(m.error), m.errorData ?? {}));
      else p.resolve(m.result);
    });
    this.#port.onDisconnect.addListener(() => {
      for (const p of this.#pending.values()) p.reject(new Error('engine disconnected'));
      this.#pending.clear();
    });
  }

  call(method, params = {}) {
    const id = ++this.#seq;
    return new Promise((resolve, reject) => {
      this.#pending.set(id, { resolve, reject });
      this.#port.postMessage({ id, method, params });
    });
  }

  /** Resolves once the engine worker has started (WASM, threads, OPFS). */
  async ready(timeoutMs = 120e3) {
    const t0 = Date.now();
    for (;;) {
      const h = await this.call('hostState');
      this.host = h;
      if (h.ready) return h;
      if (h.fatal && h.restarts >= 3) throw new Error(`engine failed: ${h.fatal}`);
      if (Date.now() - t0 > timeoutMs) throw new Error(`engine did not start: ${h.fatal ?? 'timeout'}`);
      await new Promise((r) => setTimeout(r, 250));
    }
  }

  onEvent(f) {
    this.#eventListeners.add(f);
    return () => this.#eventListeners.delete(f);
  }

  onBoard(f) {
    this.#boardListeners.add(f);
    return () => this.#boardListeners.delete(f);
  }

  onHost(f) {
    this.#hostListeners.add(f);
    return () => this.#hostListeners.delete(f);
  }

  close() {
    this.#port.disconnect();
  }
}
