// What every link to the board (Web Serial, Web Bluetooth, the test PTY)
// gives the protocol: whole frames out, whole frames in.
//
//   await link.send(type, payload)
//   const { type, payload } = await link.recv(timeoutMs)   // LinkTimeout if none
//   link.drain()        // forget frames nobody asked for
//   await link.close()
//
// Links push decoded frames into a FrameQueue; `recv` takes them in order.

export class LinkTimeout extends Error {
  constructor(ms, over = null) {
    super(`the board did not answer${over ? ` over ${over}` : ''} within ${Math.round(ms / 1000)} s`);
    this.name = 'LinkTimeout';
    this.ms = ms;
  }
}

/**
 * A timeout on a link that may not be the one the board serves: it says
 * what to check. The board answers on one link at a time, set on it
 * (Settings → Link), and a locked board does not answer the pairing.
 */
export function linkTimeoutHint(e, link) {
  if (!(e instanceof LinkTimeout)) return e;
  const out = new LinkTimeout(e.ms, link);
  out.message += `. Is it unlocked, and is its Link set to ${link} (Settings → Link)?`;
  return out;
}

export class LinkClosed extends Error {
  constructor(why) {
    super(`the link to the board closed: ${why}`);
    this.name = 'LinkClosed';
  }
}

export class FrameQueue {
  #frames = [];
  #waiter = null;
  #closed = null;

  /** A frame arrived. */
  push(frame) {
    if (this.#waiter) {
      const w = this.#waiter;
      this.#waiter = null;
      clearTimeout(w.timer);
      w.resolve(frame);
    } else {
      this.#frames.push(frame);
    }
  }

  /** The link died: every wait from now on fails with `why`. */
  close(why) {
    if (this.#closed) return;
    this.#closed = why;
    if (this.#waiter) {
      const w = this.#waiter;
      this.#waiter = null;
      clearTimeout(w.timer);
      w.reject(new LinkClosed(why));
    }
  }

  get closed() {
    return this.#closed;
  }

  drain() {
    this.#frames = [];
  }

  next(timeoutMs) {
    if (this.#frames.length) return Promise.resolve(this.#frames.shift());
    if (this.#closed) return Promise.reject(new LinkClosed(this.#closed));
    if (this.#waiter) return Promise.reject(new Error('one request at a time on the board link'));
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.#waiter = null;
        reject(new LinkTimeout(timeoutMs));
      }, timeoutMs);
      this.#waiter = { resolve, reject, timer };
    });
  }
}
