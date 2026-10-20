/**
 * When the wallet syncs: once when it opens, then every `intervalMs` while
 * the app is in the foreground, and on request (after a send or a shield,
 * or the Sync button). Never two at once: a request while one runs is
 * remembered and runs right after it, once, however many came in; a timer
 * tick while one runs is dropped (the next tick comes anyway).
 *
 * Paused while the app is in the background (Android, a minimised window):
 * no timer runs; on resume it syncs at once if the last one is older than
 * the interval, and the timer starts again.
 *
 * The timers are injectable so the tests drive the clock (test/sync-scheduler.test.ts).
 */

export interface Timers {
  set(f: () => void, ms: number): unknown;
  clear(t: unknown): void;
  now(): number;
}

const realTimers: Timers = {
  set: (f, ms) => setTimeout(f, ms),
  clear: (t) => clearTimeout(t as ReturnType<typeof setTimeout>),
  now: () => Date.now(),
};

/** About Zodl's cadence: a block is 75 s. */
export const SYNC_INTERVAL_MS = 75_000;

export class SyncScheduler {
  private readonly run: () => Promise<void>;
  private readonly intervalMs: number;
  private readonly timers: Timers;
  private timer: unknown = null;
  private active = false;
  private paused = false;
  private runningNow = false;
  private again = false;
  private lastStart = -Infinity;
  private holds = 0;
  private idleWaiters: (() => void)[] = [];

  constructor(run: () => Promise<void>, { intervalMs = SYNC_INTERVAL_MS, timers = realTimers }: { intervalMs?: number; timers?: Timers } = {}) {
    this.run = run;
    this.intervalMs = intervalMs;
    this.timers = timers;
  }

  /** A sync is running now. */
  get running(): boolean { return this.runningNow; }
  /** Started and not stopped. */
  get started(): boolean { return this.active; }

  /** Start for a wallet: sync now, then on the interval. Idempotent. */
  start(): void {
    if (this.active) return;
    this.active = true;
    void this.trigger();
  }

  /** The wallet left the screen (lock, disconnect, another wallet). */
  stop(): void {
    this.active = false;
    this.again = false;
    this.unschedule();
  }

  /**
   * Hold syncing off while a send is between propose and finish: the
   * wallet runs one command at a time per network, so a sync starting
   * after the board signed would make the finish fail. Requests meanwhile
   * are served by one sync on release. Returns the release.
   */
  hold(): () => void {
    this.holds++;
    this.unschedule();
    let released = false;
    return () => {
      if (released) return;
      released = true;
      this.holds--;
      if (this.holds > 0 || !this.active || this.paused || this.runningNow) return;
      if (this.again) { this.again = false; void this.trigger(); } else this.schedule(this.intervalMs);
    };
  }

  /** Resolves when no sync is running (at once if none is). */
  whenIdle(): Promise<void> {
    if (!this.runningNow) return Promise.resolve();
    return new Promise((r) => this.idleWaiters.push(r));
  }

  /** The app went to the background. */
  pause(): void {
    this.paused = true;
    this.unschedule();
  }

  /** Back in the foreground: sync if the last one is stale, then resume the timer. */
  resume(): void {
    if (!this.paused) return;
    this.paused = false;
    if (!this.active) return;
    if (this.timers.now() - this.lastStart >= this.intervalMs) void this.trigger();
    else this.schedule(this.intervalMs - (this.timers.now() - this.lastStart));
  }

  /**
   * Sync now. While one runs, the request is kept and served by one more
   * sync right after it (a send just made should show in the next one).
   */
  async trigger(): Promise<void> {
    if (!this.active || this.paused) return;
    if (this.runningNow || this.holds > 0) { this.again = true; return; }
    this.unschedule();
    this.runningNow = true;
    this.lastStart = this.timers.now();
    try {
      await this.run();
    } catch {
      /* The run reports its own failures (the status line); the timer
       * still runs, so a server that was down is tried again. */
    } finally {
      this.runningNow = false;
      for (const w of this.idleWaiters.splice(0)) w();
    }
    if (!this.active || this.paused || this.holds > 0) return;
    if (this.again) {
      this.again = false;
      void this.trigger();
      return;
    }
    this.schedule(this.intervalMs);
  }

  private schedule(ms: number): void {
    this.unschedule();
    if (this.holds > 0) return;
    this.timer = this.timers.set(() => { this.timer = null; void this.trigger(); }, Math.max(0, ms));
  }

  private unschedule(): void {
    if (this.timer !== null) this.timers.clear(this.timer);
    this.timer = null;
  }
}
