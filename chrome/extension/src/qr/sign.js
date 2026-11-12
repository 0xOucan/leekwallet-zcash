// Sign with QR: the air-gapped path of the Send and Shield review, the
// desktop app's flow (app/src/qr-sign.ts) for the extension.
//
//   1. The signer view proposeSend/proposeShield built (the compact PCZT,
//      the same bytes the board signs over USB or Bluetooth) goes out as
//      an animated ur:leek-pczt loop: fountain-coded frames from leek-ur
//      (src/qr/ur.js), uppercase, drawn as one big QR code on a black page.
//   2. The board scans it (Main menu → Scan, at 2x), shows the same review,
//      and on ACCEPT shows its ur:leek-zsig. The webcam (src/qr/scan.js)
//      reads that back, part by part, into leek-ur's decoder.
//   3. The .zsig goes to the engine's `finish` exactly as one signed over
//      USB: checked against the view's hash, each signature verified before
//      it is applied, proved, extracted and stored, and broadcast after the
//      same expiry and network-upgrade checks. A mainnet send still needs
//      its real-funds tick first.
//
// Speeds, from the board bench (docs/zcash/QR-BENCH.md) and leek-ur's
// display hints for a laptop or desktop screen (LCD, board at 2x):
// "Laptop screen" 50 B at 300 ms (the hints; the default), "Smaller
// codes" 40 B at 300 ms (39–40 B read in 15–19 s), and "Safe" 50 B at
// 500 ms (`qr_defaults()`, for board firmware older than 2026-11-11).
// A phone's big codes and brightness steps do not apply to a desktop page.
//
// No DOM here: the state machine runs under Node (test/qr.test.mjs), with
// the UR module and the clock injected.

export const QR_PRESETS = {
  laptop: { fragment: 50, interval: 300, label: 'Laptop screen (50 B, 300 ms)' },
  smaller: { fragment: 40, interval: 300, label: 'Smaller codes (40 B, 300 ms)' },
  safe: { fragment: 50, interval: 500, label: 'Safe (50 B, 500 ms: board firmware before 2026-11-11)' },
};
export const DEFAULT_PRESET = 'laptop';
export const QR_SETTINGS_KEY = 'leek.qr.send';

const realClock = { set: (f, ms) => setTimeout(f, ms), clear: (h) => clearTimeout(h), now: () => Date.now() };

function store(s) {
  if (s) return s;
  try {
    return globalThis.localStorage ?? null;
  } catch {
    return null;
  }
}

export function loadPreset(s) {
  try {
    const v = store(s)?.getItem(QR_SETTINGS_KEY);
    return v && Object.hasOwn(QR_PRESETS, v) ? v : DEFAULT_PRESET;
  } catch {
    return DEFAULT_PRESET;
  }
}

export function savePreset(p, s) {
  try {
    store(s)?.setItem(QR_SETTINGS_KEY, p);
  } catch {
    /* this session only */
  }
}

/** The presets with the laptop one taken from leek-ur's display hints. */
export function presetsFromHints(hints) {
  if (!hints) return QR_PRESETS;
  return {
    ...QR_PRESETS,
    laptop: { fragment: hints.fragmentLen, interval: hints.frameMs, label: `Laptop screen (${hints.fragmentLen} B, ${hints.frameMs} ms)` },
  };
}

/**
 * One QR signing, from the review to the .zsig. Phases: idle, showing (the
 * loop is up), scanning (the webcam reads the board's reply), finishing
 * (the .zsig is whole and goes to the engine), done, failed, cancelled.
 */
export class QrSigner {
  phase = 'idle';
  error = null;
  /** Why the last code read was not taken (scanning goes on). */
  note = '';
  onChange = () => {};

  #ur;
  #clock;
  #storage;
  #presets;
  #view = null;
  #encoder = null;
  #scan = null;
  #index = 0;
  #shown = 0;
  #version = 1;
  #timer = null;
  #generation = 0;
  #progress = { received: 0, total: 0 };
  #reads = 0;
  #versionFor;

  /** `ur`: the leek-chrome-ur module (src/qr/ur.js). */
  constructor(ur, { clock = realClock, storage, hints = null, versionFor } = {}) {
    this.#ur = ur;
    this.#clock = clock;
    this.#storage = storage;
    this.#presets = presetsFromHints(hints);
    this.#versionFor = versionFor;
    this.preset = loadPreset(storage);
  }

  get presets() {
    return this.#presets;
  }

  get settings() {
    return this.#presets[this.preset];
  }

  /** Start showing `view` (the signer view bytes). */
  show(view) {
    if (!(view instanceof Uint8Array)) throw new Error('the signer view must be bytes');
    this.#view = view;
    this.error = null;
    this.note = '';
    this.phase = 'showing';
    this.#restart();
  }

  /** A new encoder (new fragment size): the loop starts again at part 1. */
  #restart() {
    const gen = ++this.#generation;
    this.#stopTimer();
    this.#encoder?.free?.();
    try {
      this.#encoder = this.#ur.UrEncoder.leekPczt(this.#view, this.settings.fragment);
      this.#version = this.#versionFor ? this.#versionFor(this.#encoder.longest) : 0;
    } catch (e) {
      this.fail(e);
      return;
    }
    this.#index = 0;
    this.#shown = 1;
    this.onChange();
    this.#schedule(gen);
  }

  #schedule(gen) {
    this.#timer = this.#clock.set(() => this.#advance(gen), this.settings.interval);
  }

  #advance(gen) {
    if (gen !== this.#generation || this.phase !== 'showing') return;
    if (!this.#encoder.singlePart) {
      // The fountain keeps going past seqLen; wrap well before u32 does.
      this.#index = (this.#index + 1) % 0x40000000;
    }
    this.#shown++;
    this.onChange();
    this.#schedule(gen);
  }

  /** The frame to draw now, or null when nothing is showing. */
  frame() {
    if (this.phase !== 'showing' || !this.#encoder) return null;
    const text = this.#encoder.frame(this.#index);
    const seg = text.split('/')[1] ?? '';
    return {
      text,
      part: seg.includes('-') ? seg : '1-1',
      shown: this.#shown,
      seqLen: this.#encoder.seqLen,
      fragmentLen: this.#encoder.fragmentLen,
      version: this.#version,
      interval: this.settings.interval,
    };
  }

  setPreset(p) {
    if (!Object.hasOwn(this.#presets, p) || p === this.preset) return;
    const refetch = this.#presets[p].fragment !== this.settings.fragment;
    this.preset = p;
    savePreset(p, this.#storage);
    if (this.phase !== 'showing') return this.onChange();
    if (refetch) return this.#restart();
    // Same frames at another pace.
    this.#generation++;
    this.#stopTimer();
    this.onChange();
    this.#schedule(this.#generation);
  }

  /** The board shows its signature: stop the loop, start assembling. */
  startScan() {
    if (this.phase !== 'showing' && this.phase !== 'scanning') return;
    this.#generation++;
    this.#stopTimer();
    this.#scan?.free?.();
    this.#scan = new this.#ur.UrScan('leek-zsig');
    this.#progress = { received: 0, total: 0 };
    this.#reads = 0;
    this.note = '';
    this.phase = 'scanning';
    this.onChange();
  }

  /** Back to the loop (the board had not read it yet). */
  showAgain() {
    if (this.phase !== 'scanning' || !this.#view) return;
    this.phase = 'showing';
    this.#restart();
  }

  progress() {
    return { ...this.#progress, reads: this.#reads };
  }

  /**
   * One decoded QR text from the camera. Returns the .zsig (bytes) once
   * the reply is whole (the phase is then "finishing"), else null.
   */
  feed(text) {
    if (this.phase !== 'scanning' || !this.#scan) return null;
    this.#reads++;
    const state = this.#scan.feed(String(text));
    if (state === 'ignored') return null;
    this.#progress = { received: this.#scan.received, total: this.#scan.total };
    if (state === 'rejected') this.note = this.#scan.error ?? 'a QR code that is not the board’s reply';
    else if (state === 'accepted') this.note = '';
    if (state === 'complete') {
      const zsig = this.#scan.takeMessage();
      if (!zsig?.length) {
        this.note = 'the board’s reply was empty';
        this.onChange();
        return null;
      }
      this.#progress = { received: this.#progress.total || 1, total: this.#progress.total || 1 };
      this.phase = 'finishing';
      this.onChange();
      return zsig;
    }
    this.onChange();
    return null;
  }

  /** The .zsig was handed on and the send finished. */
  finished() {
    this.phase = 'done';
    this.#view = null;
    this.onChange();
  }

  fail(e) {
    this.#generation++;
    this.#stopTimer();
    this.phase = 'failed';
    this.error = e instanceof Error ? e.message : String(e?.message ?? e);
    this.onChange();
  }

  cancel() {
    this.#generation++;
    this.#stopTimer();
    this.phase = 'cancelled';
    this.#view = null;
    this.onChange();
  }

  #stopTimer() {
    if (this.#timer !== null) this.#clock.clear(this.#timer);
    this.#timer = null;
  }
}

/** The status line under the code. */
export function frameCaption(f) {
  return `Part ${f.part} · frame ${f.shown} · ${f.seqLen} fragment${f.seqLen === 1 ? '' : 's'} of ${f.fragmentLen} B${f.version ? ` · QR version ${f.version}` : ''} · ${f.interval} ms`;
}

/** The scan's progress line. */
export function scanCaption(p, note) {
  const base =
    p.total === 0
      ? 'Point the webcam at the board’s signature QR (ur:leek-zsig)…'
      : `Signature: ${p.received} of ${p.total} part${p.total === 1 ? '' : 's'} read…`;
  return note ? `${base} (ignored one code: ${note})` : base;
}
