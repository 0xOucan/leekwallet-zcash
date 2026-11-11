/**
 * Sign with QR (EXPERIMENTAL, opt-in): the air-gapped path of the Send and
 * Shield review. USB and Bluetooth stay the everyday links; this is for a
 * board kept off every link.
 *
 *   1. The signer view zw_propose_* built (the compact PCZT, the same bytes
 *      the board signs over USB) goes out as an animated ur:leek-pczt loop:
 *      fountain-coded parts from the backend (src-tauri/src/airgap.rs over
 *      host/leek-ur), uppercase, drawn here as one big QR code.
 *   2. The board scans it, shows the same review, and on ACCEPT shows its
 *      ur:leek-zsig. The camera (src/qr-scan.ts) reads that back, part by
 *      part, into the backend's decoder.
 *   3. The .zsig goes to zw_finish exactly as one signed over USB: checked
 *      against the view's hash and each signature against its rk, proved,
 *      and broadcast after the same network-upgrade and expiry checks. A
 *      mainnet send still needs its real-funds confirmation.
 *
 * The large-QR rules, from the Zodl fork's LeekBigQr (the one signed with
 * on the board): as large as the screen allows (min of 96 % of the width
 * and 70 % of the height), error correction L, a whole number of pixels per
 * module, a four-module quiet zone, black on white. And one QR version for
 * the whole loop, so the code does not change size while it animates.
 *
 * Defaults: 90-byte fragments at 500 ms (a 1.4 KB compact view is 16
 * frames, 8 s a pass), where the Ethereum LeekWallet's board benches on
 * this same board settled, with their rule: a frame stays up at least twice
 * the board camera's capture interval (300 ms made it miss most frames).
 * The Zodl fork signed at 50 B / 500 ms; 50 is one "Bigger" away.
 *
 * Speed: Safe (50/500), Balanced (50/400), Fast (50/300), or Custom with
 * Bigger / Denser / Slower / Faster, for the camera-tuning sweep (the board
 * scans at ~7 fps in QVGA today; Balanced and Fast wait for its zoom). The
 * same presets are `zecz qr-show --sweep`'s first ones; results go in
 * docs/zcash/QR-BENCH.md. The choice is remembered (leek.qr.send).
 *
 * This file is the state machine and the drawing, with the backend and the
 * clock injected: tested under Node (test/qr-sign.test.ts).
 */

import type { Invoke } from "./tauri-transport.ts";

/** Fragment sizes offered, bytes (airgap.rs FRAGMENTS). */
export const QR_FRAGMENTS = [30, 40, 50, 90, 120, 150, 200, 250] as const;
/** Frame intervals offered, ms (airgap.rs INTERVALS). */
export const QR_INTERVALS = [800, 500, 400, 350, 300, 250, 200] as const;
/** The speed presets; anything else is "custom". */
/* The board bench (2026-11-10): 90 B (QR v8) from a laptop screen was found
 * and almost never decoded; 50 B (v6) read. The presets keep 50 B and step
 * the pace; Bigger goes to 40 and 30 B. */
export const QR_PRESETS = {
  safe: { fragment: 50, interval: 500 },
  balanced: { fragment: 50, interval: 400 },
  fast: { fragment: 50, interval: 300 },
} as const;
export type QrPreset = keyof typeof QR_PRESETS | "custom" | "manual";

/**
 * The bench sweep's settings, in order (docs/zcash/QR-BENCH.md): the module
 * size first at a pace the board keeps up with, then the pace at the sizes
 * that read, and 90 B as the control that did not. Each row is timed from
 * its loop's first frame to the board's review; the table is what other
 * companions and forks take their defaults from.
 */
export const QR_BENCH_CONFIGS: readonly QrSettings[] = [
  { fragment: 30, interval: 500 }, { fragment: 40, interval: 500 }, { fragment: 50, interval: 500 },
  { fragment: 50, interval: 400 }, { fragment: 40, interval: 400 }, { fragment: 30, interval: 400 },
  { fragment: 50, interval: 300 }, { fragment: 40, interval: 300 }, { fragment: 30, interval: 300 },
  { fragment: 30, interval: 250 }, { fragment: 90, interval: 500 },
];

/** One bench row: a setting and how long the board took, or null if it gave up. */
export interface QrBenchRow {
  fragment: number;
  interval: number;
  seqLen: number;
  version: number;
  ms: number | null;
  /** Frames the loop showed before the board had it. */
  frames: number;
}
export const QR_SETTINGS_KEY = "leek.qr.send";
/** Frames fetched from the backend at a time. */
export const QR_BATCH = 64;

/** QR alphanumeric capacity at error correction L, versions 1-40. */
const ALNUM_L = [
  25, 47, 77, 114, 154, 195, 224, 279, 335, 395, 468, 535, 619, 667, 758, 854, 938, 1046, 1153, 1249,
  1352, 1460, 1588, 1704, 1853, 1990, 2132, 2223, 2369, 2520, 2677, 2840, 3009, 3183, 3351, 3537, 3729, 3927, 4087, 4296,
];

/** The smallest QR version (error correction L) that holds `chars` alphanumeric characters. */
export function qrVersionFor(chars: number): number {
  const i = ALNUM_L.findIndex((c) => c >= chars);
  if (i < 0) throw new Error(`a QR frame of ${chars} characters is too long`);
  return i + 1;
}

/** Pixels per module and the drawn side for a code of `modules` (quiet zone included) in `avail` px. */
export function moduleScale(modules: number, avail: number): { px: number; side: number } {
  const px = Math.max(1, Math.floor(avail / modules));
  return { px, side: px * modules };
}

/** The large-QR rule: min(96 % of the width, 70 % of the height). */
export function bigQrAvail(width: number, height: number): number {
  return Math.floor(Math.min(width * 0.96, height * 0.7));
}

/**
 * Full screen: the whole of the smaller side, less `reserve` px of the
 * height for the captions under the code. drawQr then fits a whole number
 * of pixels per module, quiet zone included, inside it.
 */
export function fullQrAvail(width: number, height: number, reserve: number): number {
  return Math.max(1, Math.floor(Math.min(width, height - Math.max(0, reserve))));
}

/** `manual`: one frame per Next (button or Space), for debugging a camera. */
export interface QrSettings { fragment: number; interval: number; manual?: boolean }

type Store = Pick<Storage, "getItem" | "setItem">;
function store(s?: Store): Store | null {
  if (s) return s;
  try { return globalThis.localStorage ?? null; } catch { return null; }
}

export function loadQrSettings(s?: Store): QrSettings {
  const def: QrSettings = { ...QR_PRESETS.safe };
  try {
    const v = JSON.parse(store(s)?.getItem(QR_SETTINGS_KEY) ?? "null") as Partial<QrSettings> | null;
    const fragment = (QR_FRAGMENTS as readonly number[]).includes(v?.fragment ?? -1) ? v!.fragment! : def.fragment;
    const interval = (QR_INTERVALS as readonly number[]).includes(v?.interval ?? -1) ? v!.interval! : def.interval;
    return { fragment, interval, manual: v?.manual === true };
  } catch {
    return def;
  }
}

export function saveQrSettings(q: QrSettings, s?: Store): void {
  try { store(s)?.setItem(QR_SETTINGS_KEY, JSON.stringify(q)); } catch { /* this session only */ }
}

interface QrFrames { seqLen: number; fragmentLen: number; longest: number; frames: string[] }
export interface ScanProgress {
  state: "accepted" | "redundant" | "ignored" | "rejected" | "complete";
  received: number;
  total: number;
  error: string | null;
  zsig: number[] | null;
}

export type QrPhase = "idle" | "showing" | "scanning" | "finishing" | "done" | "failed" | "cancelled";

/** What the screen draws now. */
export interface QrFrameView {
  text: string;
  /** "12-28" for a multi-part frame, "1-1" for a still code. */
  part: string;
  /** Frames shown since the loop (re)started, 1-based. */
  shown: number;
  seqLen: number;
  fragmentLen: number;
  version: number;
  interval: number;
  manual: boolean;
}

export interface Clock {
  set(fn: () => void, ms: number): unknown;
  clear(handle: unknown): void;
  /** Milliseconds, for the stopwatch; Date.now when left out. */
  now?(): number;
}
const realClock: Clock = {
  set: (fn, ms) => setTimeout(fn, ms), clear: (h) => clearTimeout(h as ReturnType<typeof setTimeout>), now: () => Date.now(),
};

/**
 * The bench stopwatch (docs/zcash/QR-BENCH.md): how long the board took to
 * read the transaction (from the loop's start, restarted by every speed
 * change, to "the board shows its signature"), how long the camera took to
 * read the reply, and the settings that did it. Null until measured.
 */
export interface QrTiming {
  fragment: number;
  interval: number;
  manual: boolean;
  seqLen: number;
  version: number;
  /** The loop showing, ms: the board's scan, ended by the user's click. */
  showMs: number | null;
  /** Reading the board's reply, ms, until the .zsig was whole. */
  readMs: number | null;
  /** QR codes the camera decoded during the read, the board's or not. */
  qrReads: number;
}

/**
 * One QR signing, from the review to the .zsig. The screen subscribes with
 * `onChange` and draws `frame()` / `progress()`.
 */
export class QrSigner {
  phase: QrPhase = "idle";
  settings: QrSettings;
  error: string | null = null;
  /** The last thing the scan said, for the status line. */
  scanNote = "";
  onChange: () => void = () => {};

  private view: Uint8Array | null = null;
  private frames: string[] = [];
  private batchStart = 0;
  private index = 0;
  private shown = 0;
  private seqLen = 0;
  private fragmentLen = 0;
  private version = 1;
  private timer: unknown = null;
  private generation = 0;
  private scanProgress = { received: 0, total: 0 };
  private feeding: Promise<void> = Promise.resolve();
  private zsig: Uint8Array | null = null;
  private shownAt: number | null = null;
  private scanAt: number | null = null;
  private doneAt: number | null = null;
  private showMs: number | null = null;
  private qrReads = 0;
  private timedSettings: QrSettings | null = null;
  /** The sweep in progress: the index into QR_BENCH_CONFIGS, and its rows. */
  bench: { at: number; rows: QrBenchRow[] } | null = null;

  private readonly invoke: Invoke;
  private readonly clock: Clock;
  private readonly storage: Store | undefined;

  constructor(invoke: Invoke, clock: Clock = realClock, storage?: Store) {
    this.invoke = invoke;
    this.clock = clock;
    this.storage = storage;
    this.settings = loadQrSettings(storage);
  }

  /** Start showing `view` (the signer view from zw_propose_*). */
  async show(view: Uint8Array): Promise<void> {
    this.stopTimer();
    this.view = view;
    this.error = null;
    this.zsig = null;
    this.phase = "showing";
    await this.restart();
  }

  /** Same view, new settings: a new encoder, so the loop starts at part 1. */
  private async restart(): Promise<void> {
    const gen = ++this.generation;
    this.stopTimer();
    this.frames = [];
    this.batchStart = 0;
    this.index = 0;
    this.shown = 0;
    /* A new loop is a new measurement: the board starts again from part 1. */
    this.shownAt = null;
    try {
      await this.fetch(0, gen);
    } catch (e) {
      if (gen === this.generation) this.fail(e);
      return;
    }
    if (gen !== this.generation || this.phase !== "showing") return;
    this.shown = 1;
    this.shownAt = this.now();
    this.scanAt = this.doneAt = this.showMs = null;
    this.timedSettings = { ...this.settings };
    this.onChange();
    this.schedule(gen);
  }

  private async fetch(start: number, gen: number): Promise<void> {
    const view = this.view;
    if (!view) throw new Error("No transaction to show.");
    const r = await this.invoke<QrFrames>("qr_pczt_frames", {
      view: Array.from(view), maxFragment: this.settings.fragment, start, count: QR_BATCH,
    });
    if (gen !== this.generation) return;
    if (!r.frames.length) throw new Error("the backend returned no QR frames");
    this.frames = r.frames;
    this.batchStart = start;
    this.seqLen = r.seqLen;
    this.fragmentLen = r.fragmentLen;
    this.version = qrVersionFor(r.longest);
  }

  private schedule(gen: number): void {
    if (this.settings.manual) return;   /* next() steps instead */
    this.timer = this.clock.set(() => void this.advance(gen), this.settings.interval);
  }

  /** Manual mode: show the next part now. */
  next(): void {
    if (this.phase === "showing" && this.settings.manual) void this.advance(this.generation);
  }

  private async advance(gen: number): Promise<void> {
    if (gen !== this.generation || this.phase !== "showing") return;
    if (this.seqLen > 1) {
      this.index++;
      if (this.index >= this.frames.length) {
        try {
          await this.fetch(this.batchStart + this.frames.length, gen);
        } catch (e) {
          if (gen === this.generation) this.fail(e);
          return;
        }
        if (gen !== this.generation || this.phase !== "showing") return;
        this.index = 0;
      }
    }
    this.shown++;
    this.onChange();
    this.schedule(gen);
  }

  /** The frame to draw, or null when nothing is showing. */
  frame(): QrFrameView | null {
    if (this.phase !== "showing" || !this.frames.length) return null;
    const text = this.frames[this.index]!;
    const part = text.split("/")[1]?.includes("-") ? text.split("/")[1]! : "1-1";
    return {
      text, part, shown: this.shown, seqLen: this.seqLen, fragmentLen: this.fragmentLen,
      version: this.version, interval: this.settings.interval, manual: !!this.settings.manual,
    };
  }

  private step<T extends number>(list: readonly T[], now: number, by: number): T {
    const i = Math.max(0, list.indexOf(now as T));
    return list[Math.min(list.length - 1, Math.max(0, i + by))]!;
  }

  private change(next: QrSettings): void {
    if (next.fragment === this.settings.fragment && next.interval === this.settings.interval &&
        !!next.manual === !!this.settings.manual) return;
    const refetch = next.fragment !== this.settings.fragment;
    this.settings = next;
    saveQrSettings(next, this.storage);
    if (this.phase !== "showing") { this.onChange(); return; }
    if (refetch) void this.restart();
    else {
      /* Same frames at another pace: timed again from now. */
      this.shownAt = this.now();
      this.timedSettings = { ...next };
      this.stopTimer(); this.onChange(); this.schedule(this.generation);
    }
  }

  /** Which preset the settings are, or "custom". */
  preset(): QrPreset {
    if (this.settings.manual) return "manual";
    for (const [k, v] of Object.entries(QR_PRESETS)) {
      if (v.fragment === this.settings.fragment && v.interval === this.settings.interval) return k as QrPreset;
    }
    return "custom";
  }

  /** Safe, Balanced or Fast. "custom" keeps the settings as they are. */
  setPreset(p: QrPreset): void {
    if (p === "custom") return;
    if (p === "manual") { this.change({ ...this.settings, manual: true }); return; }
    this.change({ ...QR_PRESETS[p] });
  }

  /** Bigger modules: smaller fragments, more frames (down to 50 B). */
  bigger(): void { this.change({ ...this.settings, fragment: this.step(QR_FRAGMENTS, this.settings.fragment, -1) }); }
  /** Denser codes: larger fragments, fewer frames. */
  denser(): void { this.change({ ...this.settings, fragment: this.step(QR_FRAGMENTS, this.settings.fragment, +1) }); }
  /* A pace is automatic: Slower and Faster leave manual mode. */
  slower(): void { this.change({ ...this.settings, manual: false, interval: this.step(QR_INTERVALS, this.settings.interval, -1) }); }
  faster(): void { this.change({ ...this.settings, manual: false, interval: this.step(QR_INTERVALS, this.settings.interval, +1) }); }
  canBigger(): boolean { return this.settings.fragment !== QR_FRAGMENTS[0]; }
  canDenser(): boolean { return this.settings.fragment !== QR_FRAGMENTS[QR_FRAGMENTS.length - 1]; }
  canSlower(): boolean { return this.settings.interval !== QR_INTERVALS[0]; }
  canFaster(): boolean { return this.settings.interval !== QR_INTERVALS[QR_INTERVALS.length - 1]; }

  /** The board showed its reply: stop the loop, start assembling. */
  async startScan(): Promise<void> {
    if (this.phase !== "showing" && this.phase !== "scanning") return;
    this.generation++;
    this.stopTimer();
    if (this.phase === "showing") {
      this.scanAt = this.now();
      this.showMs = this.shownAt === null ? null : this.scanAt - this.shownAt;
      this.doneAt = null;
      this.qrReads = 0;
    }
    await this.invoke<void>("qr_scan_reset");
    this.scanProgress = { received: 0, total: 0 };
    this.scanNote = "";
    this.phase = "scanning";
    this.onChange();
  }

  /** Back to showing the PCZT (the board had not scanned it yet). */
  async showAgain(): Promise<void> {
    if (this.phase !== "scanning" || !this.view) return;
    this.phase = "showing";
    await this.restart();
  }

  progress(): { received: number; total: number } {
    return { ...this.scanProgress };
  }

  /**
   * One decoded QR text from the camera, fed in arrival order. Resolves to
   * the .zsig once the reply is complete (then the phase is "finishing"),
   * else null.
   */
  feed(text: string): Promise<Uint8Array | null> {
    const run = this.feeding.then(async (): Promise<Uint8Array | null> => {
      if (this.phase !== "scanning") return null;
      this.qrReads++;
      const p = await this.invoke<ScanProgress>("qr_scan_feed", { text });
      if (this.phase !== "scanning") return null;
      if (p.state === "ignored") return null;
      this.scanProgress = { received: p.received, total: p.total };
      if (p.state === "rejected") this.scanNote = p.error ?? "a QR code that is not the board's reply";
      else if (p.state === "accepted") this.scanNote = "";
      if (p.state === "complete" && p.zsig) {
        this.zsig = Uint8Array.from(p.zsig);
        this.doneAt = this.now();
        this.phase = "finishing";
        this.onChange();
        return this.zsig;
      }
      this.onChange();
      return null;
    });
    this.feeding = run.then(() => undefined, () => undefined);
    return run;
  }

  /** The .zsig was handed on and the send finished. */
  finished(): void {
    this.phase = "done";
    this.view = null;
    this.onChange();
  }

  fail(e: unknown): void {
    this.generation++;
    this.stopTimer();
    this.phase = "failed";
    this.error = e instanceof Error ? e.message : String(e);
    this.onChange();
  }

  cancel(): void {
    this.generation++;
    this.stopTimer();
    this.phase = "cancelled";
    this.view = null;
    this.frames = [];
    this.onChange();
  }

  /** Start the bench sweep from its first setting. */
  startBench(): void {
    if (this.phase !== "showing") return;
    this.bench = { at: 0, rows: [] };
    const same = QR_BENCH_CONFIGS[0]!.fragment === this.settings.fragment;
    this.change({ ...QR_BENCH_CONFIGS[0]! });
    if (same) void this.restart();
  }

  /**
   * The board has it (its review is up), or `gaveUp`: record this setting's
   * row and move to the next. False once the sweep is over.
   */
  benchNext(gaveUp = false): boolean {
    const b = this.bench;
    if (!b || this.phase !== "showing") return false;
    const t = this.timing();
    b.rows.push({
      fragment: t.fragment, interval: t.interval, seqLen: t.seqLen, version: t.version,
      ms: gaveUp ? null : t.showMs, frames: this.shown,
    });
    b.at++;
    if (b.at >= QR_BENCH_CONFIGS.length) { this.onChange(); return false; }
    const next = QR_BENCH_CONFIGS[b.at]!;
    const same = next.fragment === this.settings.fragment;
    this.change({ ...next });
    /* Same fragment size, new pace: the board scans again from part 1, so
       the loop does too. */
    if (same) void this.restart();
    return true;
  }

  /** The stopwatch so far (live while a phase runs). */
  timing(): QrTiming {
    const s = this.timedSettings ?? this.settings;
    const now = this.now();
    const showMs = this.showMs ?? (this.phase === "showing" && this.shownAt !== null ? now - this.shownAt : null);
    const readMs = this.scanAt === null ? null : (this.doneAt ?? (this.phase === "scanning" ? now : this.scanAt)) - this.scanAt;
    return { fragment: s.fragment, interval: s.interval, manual: !!s.manual, seqLen: this.seqLen, version: this.version, showMs, readMs, qrReads: this.qrReads };
  }

  private now(): number {
    return this.clock.now ? this.clock.now() : Date.now();
  }

  private stopTimer(): void {
    if (this.timer !== null) this.clock.clear(this.timer);
    this.timer = null;
  }
}

/** The status line under the code. */
export function frameCaption(f: QrFrameView): string {
  return `Part ${f.part} · frame ${f.shown} · ${f.seqLen} fragment${f.seqLen === 1 ? "" : "s"} of ${f.fragmentLen} B · QR version ${f.version} · ${f.manual ? "manual: Next or Space" : `${f.interval} ms`}`;
}

const secs = (ms: number | null) => (ms === null ? "–" : `${(ms / 1000).toFixed(1)} s`);

/**
 * The stopwatch line: on screen while it runs, and once a signing is read
 * whole, the line the app logs for docs/zcash/QR-BENCH.md.
 */
export function timingCaption(t: QrTiming): string {
  const total = t.showMs !== null && t.readMs !== null ? ` · total ${secs(t.showMs + t.readMs)}` : "";
  return `⏱ ${t.fragment} B / ${t.manual ? "manual" : `${t.interval} ms`} (${t.seqLen} fragment${t.seqLen === 1 ? "" : "s"}, QR v${t.version}) · board read ${secs(t.showMs)}`
    + ` · reply read ${secs(t.readMs)}${t.readMs !== null ? ` (${t.qrReads} QR)` : ""}${total}`;
}

/** The bench rows as a Markdown table, for QR-BENCH.md and the forks. */
export function benchTable(rows: readonly QrBenchRow[], note = ""): string {
  const head = "| Fragment B | Interval ms | Parts | QR v | Board read s | Frames shown | Loops |\n|---|---|---|---|---|---|---|";
  const body = rows.map((r) => `| ${r.fragment} | ${r.interval} | ${r.seqLen} | ${r.version} | ${r.ms === null ? "gave up" : (r.ms / 1000).toFixed(1)} | ${r.frames} | ${(r.frames / Math.max(1, r.seqLen)).toFixed(1)} |`);
  return [note ? `${note}\n` : "", head, ...body].filter(Boolean).join("\n");
}

/** The scan's progress line. */
export function scanCaption(p: { received: number; total: number }, note: string): string {
  const base = p.total === 0
    ? "Point the camera at the board's signature QR (ur:leek-zsig)…"
    : `Signature: ${p.received} of ${p.total} part${p.total === 1 ? "" : "s"} read…`;
  return note ? `${base} (ignored one code: ${note})` : base;
}

type QrGen = (type: number, ec: "L" | "M" | "Q" | "H") => {
  addData(d: string, mode?: "Alphanumeric"): void; make(): void; getModuleCount(): number; isDark(r: number, c: number): boolean;
};

/**
 * Draw `text` as a QR code on `canvas`: version `version`, error correction
 * L, alphanumeric mode, a four-module quiet zone, black on white, a whole
 * number of pixels per module in `avail` CSS px (times the device pixel
 * ratio, so a module is whole device pixels too).
 */
export function drawQr(canvas: HTMLCanvasElement, gen: QrGen, text: string, version: number, avail: number, dpr = 1): void {
  const qr = gen(version, "L");
  qr.addData(text, "Alphanumeric");
  qr.make();
  const n = qr.getModuleCount();
  const quiet = 4;
  const { px, side } = moduleScale(n + quiet * 2, Math.floor(avail * dpr));
  if (canvas.width !== side) { canvas.width = side; canvas.height = side; }
  canvas.style.width = `${side / dpr}px`;
  canvas.style.height = `${side / dpr}px`;
  const ctx = canvas.getContext("2d");
  if (!ctx) return;
  ctx.imageSmoothingEnabled = false;
  ctx.fillStyle = "#ffffff";
  ctx.fillRect(0, 0, side, side);
  ctx.fillStyle = "#000000";
  for (let y = 0; y < n; y++) {
    for (let x = 0; x < n; x++) {
      if (qr.isDark(y, x)) ctx.fillRect((x + quiet) * px, (y + quiet) * px, px, px);
    }
  }
}
