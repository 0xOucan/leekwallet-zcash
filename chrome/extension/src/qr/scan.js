// The webcam, for the board's codes: its signature (ur:leek-zsig) in Sign
// with QR, and its account (ur:leek-zcash-accounts, Main menu → View key)
// in Import from the board's QR. Never for pairing.
//
// The desktop app's scanner (app/src/qr-scan.ts and qr-tuning.ts) in plain
// JavaScript: the same decoder, zxing-wasm's reader (MIT, the version the
// app pins, vendored at build time from the pinned npm package into
// src/vendor/zxing-wasm, its .wasm served from the extension itself; the
// CDN default is replaced, so nothing is fetched), the same decode passes
// and software contrast. Why zxing-wasm and not BarcodeDetector or jsQR:
// qr-scan.ts says (jsQR could not read dense codes off a screen; the app
// keeps one decoder on every platform).
//
// getUserMedia runs in the extension's tab (popup.html?tab): Chrome asks
// for the camera there, once per extension origin. The toolbar popup
// closes on blur, so it opens the tab instead. Frames go from the <video>
// to a canvas and into the decoder; nothing is stored or sent, and the
// camera is released on stop.

let prepared = null;
let zxing = null;

/**
 * The reader, prepared once. `overrides` (tests under Node: `{ wasmBinary }`)
 * replace the default, which loads the packaged .wasm next to the module.
 */
export async function reader(overrides = null) {
  zxing ??= await import('../vendor/zxing-wasm/es/reader/index.js');
  prepared ??= zxing.prepareZXingModule({
    overrides: overrides ?? { locateFile: (path) => new URL(`../vendor/zxing-wasm/reader/${path}`, import.meta.url).href },
    fireImmediately: true,
  });
  await prepared;
  return zxing;
}

/** Whether this page can reach a camera at all. */
export function scanningAvailable() {
  return typeof navigator?.mediaDevices?.getUserMedia === 'function';
}

// ------------------------------------------------- tuning (qr-tuning.ts)

export const CAMERA_KEY = 'leek.qr.camera';

function store(s) {
  if (s) return s;
  try {
    return globalThis.localStorage ?? null;
  } catch {
    return null;
  }
}

/** The camera to open: `{ deviceId }`, or `{ any: true }` (the default webcam). */
export function loadCamera(s) {
  try {
    const v = store(s)?.getItem(CAMERA_KEY) ?? '';
    if (v.startsWith('id:') && v.length > 3 && v.length < 300) return { deviceId: v.slice(3) };
  } catch {
    /* the default */
  }
  return { any: true };
}

export function saveCamera(c, s) {
  try {
    if (c.deviceId) store(s)?.setItem(CAMERA_KEY, `id:${c.deviceId}`);
  } catch {
    /* this scan only */
  }
}

/** Full HD asked, not demanded. A laptop's webcam has no facing mode. */
export function videoConstraints(c) {
  const size = { width: { ideal: 1920 }, height: { ideal: 1080 } };
  return c.deviceId ? { deviceId: { exact: c.deviceId }, ...size } : size;
}

/** The camera after `current` among `devices` (ids are listed once the camera was allowed). */
export function nextCamera(devices, currentId) {
  const ids = devices.map((d) => d.deviceId).filter((id) => id !== '');
  if (ids.length < 2) return null;
  const i = ids.indexOf(currentId ?? '');
  return { deviceId: ids[(i + 1) % ids.length] };
}

/** A square in the middle of a w x h frame, `fraction` of its short side. */
export function centerCrop(w, h, fraction = 0.6) {
  const size = Math.max(1, Math.round(Math.min(w, h) * Math.min(1, Math.max(0.1, fraction))));
  return { sx: Math.floor((w - size) / 2), sy: Math.floor((h - size) / 2), size };
}

/**
 * The passes for frame `n` (qr-tuning.ts decodePasses): the middle at full
 * resolution every frame, plus one that rotates. For an animated reply, a
 * quick attempt every frame beats a thorough one on a few: a missed frame
 * comes round again.
 */
export function decodePasses(n, darken = 0, animated = false) {
  const always = darken < 0;
  if (animated && n % 3 !== 0) return [{ region: 'center', binarizer: 'LocalAverage', contrast: always }];
  const k = animated ? Math.floor(n / 3) : n;
  const extra = [
    { region: 'full', binarizer: 'LocalAverage', contrast: always },
    { region: 'center', binarizer: 'LocalAverage', contrast: true },
    { region: 'center', binarizer: 'GlobalHistogram', contrast: always },
  ];
  return [{ region: 'center', binarizer: 'LocalAverage', contrast: always }, extra[((k % extra.length) + extra.length) % extra.length]];
}

/**
 * Grey, stretched and darkened in place (qr-tuning.ts enhance): the 2nd to
 * 98th luminance percentile stretched to the full range, then a gamma
 * above 1 pulls the glow of a bright screen off the modules. `darken` 0 to
 * -3. RGBA; alpha untouched.
 */
export function enhance(px, darken = 0) {
  const n = px.length >> 2;
  if (n === 0) return;
  const hist = new Uint32Array(256);
  for (let i = 0; i < n; i++) {
    const o = i << 2;
    const y = (px[o] * 77 + px[o + 1] * 150 + px[o + 2] * 29) >> 8;
    px[o] = y;
    hist[y]++;
  }
  let acc = 0;
  let lo = -1;
  let hi = -1;
  for (let v = 0; v < 256; v++) {
    acc += hist[v];
    if (lo < 0 && acc > n * 0.02) lo = v;
    if (hi < 0 && acc >= n * 0.98) hi = v;
  }
  if (lo < 0) lo = 0;
  if (hi < 0) hi = 255;
  if (hi <= lo) hi = Math.min(255, lo + 1);
  const gamma = 1 + Math.min(3, Math.max(0, -darken)) * 0.6;
  const lut = new Uint8ClampedArray(256);
  for (let v = 0; v < 256; v++) lut[v] = Math.round(255 * Math.pow(Math.min(1, Math.max(0, (v - lo) / (hi - lo))), gamma));
  for (let i = 0; i < n; i++) {
    const o = i << 2;
    const g = lut[px[o]];
    px[o] = px[o + 1] = px[o + 2] = g;
  }
}

/** Decode the QR codes in one RGBA image (`{ data, width, height }`). */
export async function decodeImage(img, { binarizer = 'LocalAverage' } = {}) {
  const z = await reader();
  const found = await z.readBarcodes(img, { formats: ['QRCode'], tryHarder: true, tryInvert: true, binarizer, maxNumberOfSymbols: 4 });
  return found.map((f) => f.text.trim()).filter(Boolean);
}

// ------------------------------------------------- the camera

/** Longest edge of the whole frame handed to the decoder. */
const DECODE_MAX_EDGE = 1920;
/** Shortest gap between two decode attempts: an animated code does not wait. */
const ANIMATED_INTERVAL_MS = 30;
const STILL_INTERVAL_MS = 120;

async function open(choice) {
  try {
    return await navigator.mediaDevices.getUserMedia({ video: videoConstraints(choice), audio: false });
  } catch (e) {
    // A remembered camera that is gone: the default one.
    if (choice.deviceId && ['OverconstrainedError', 'NotFoundError', 'NotReadableError'].includes(e?.name)) {
      return navigator.mediaDevices.getUserMedia({ video: videoConstraints({ any: true }), audio: false });
    }
    throw e;
  }
}

/** What the browser said, as a line for the user. */
export function cameraError(e) {
  const name = e?.name ?? '';
  if (name === 'NotAllowedError' || name === 'SecurityError') {
    return 'The camera was not allowed. Allow it for this extension (the camera icon in the address bar, or the site settings of this page) and press Scan again.';
  }
  if (name === 'NotFoundError' || name === 'OverconstrainedError') return 'No camera was found. Plug in a webcam and press Scan again.';
  if (name === 'NotReadableError') return 'The camera is in use by another program. Close it and press Scan again.';
  return `The camera did not open: ${e?.message ?? e}`;
}

/**
 * Scan with the camera into `video` until `accept(text)` returns
 * something other than undefined for a decoded text; then stop and hand it
 * to `onResult`. Codes `accept` ignores do not end the scan. Returns
 * `{ stop, cameras, switchCamera }`.
 */
export async function scanQr(video, accept, onResult, onError, { animated = true } = {}) {
  if (!scanningAvailable()) throw new Error('This page cannot reach a camera.');
  const gap = animated ? ANIMATED_INTERVAL_MS : STILL_INTERVAL_MS;
  const loading = reader();
  let choice = loadCamera();
  let stream = await open(choice);
  let stopped = false;
  let timer = null;
  let devices = [];

  const stop = () => {
    if (stopped) return;
    stopped = true;
    if (timer) clearTimeout(timer);
    for (const t of stream.getTracks()) t.stop();
    video.srcObject = null;
  };
  const attach = async () => {
    video.srcObject = stream;
    video.muted = true;
    video.setAttribute('playsinline', '');
    await video.play().catch(() => undefined);
    try {
      devices = (await navigator.mediaDevices.enumerateDevices()).filter((d) => d.kind === 'videoinput');
    } catch {
      devices = [];
    }
  };
  const canvas = document.createElement('canvas');
  const ctx = canvas.getContext('2d', { willReadFrequently: true });
  let z;
  try {
    if (!ctx) throw new Error('This page cannot open a canvas to read the camera.');
    await attach();
    z = await loading;
  } catch (e) {
    stop();
    throw e;
  }

  const grab = (region, contrast) => {
    const w = video.videoWidth;
    const h = video.videoHeight;
    if (!(w > 0 && h > 0)) return null;
    let sx = 0;
    let sy = 0;
    let sw = w;
    let sh = h;
    let dw;
    let dh;
    if (region === 'center') {
      const c = centerCrop(w, h);
      ({ sx, sy } = c);
      sw = sh = dw = dh = c.size;
    } else {
      const scale = Math.min(1, DECODE_MAX_EDGE / Math.max(w, h));
      dw = Math.round(w * scale);
      dh = Math.round(h * scale);
    }
    if (canvas.width !== dw || canvas.height !== dh) {
      canvas.width = dw;
      canvas.height = dh;
    }
    ctx.drawImage(video, sx, sy, sw, sh, 0, 0, dw, dh);
    const img = ctx.getImageData(0, 0, dw, dh);
    if (contrast) enhance(img.data, 0);
    return img;
  };

  let failures = 0;
  let frame = 0;
  const tick = async () => {
    if (stopped) return;
    try {
      for (const pass of decodePasses(frame++, 0, animated)) {
        if (stopped) return;
        const img = grab(pass.region, pass.contrast);
        if (!img) break;
        const found = await z.readBarcodes(img, { formats: ['QRCode'], tryHarder: true, tryInvert: true, binarizer: pass.binarizer, maxNumberOfSymbols: 4 });
        failures = 0;
        for (const f of found) {
          const v = accept(f.text.trim());
          if (v !== undefined && !stopped) {
            stop();
            onResult(v);
            return;
          }
        }
      }
    } catch (e) {
      // One bad frame is not the end of a scan; a decoder failing every frame is.
      if (++failures >= 20) {
        stop();
        onError(`The QR decoder failed: ${e?.message ?? e}`);
        return;
      }
    }
    if (!stopped) timer = setTimeout(() => void tick(), gap);
  };
  timer = setTimeout(() => void tick(), gap);

  return {
    stop,
    cameras: () => devices.filter((d) => d.deviceId !== '').length,
    async switchCamera() {
      if (stopped) return;
      const current = stream.getVideoTracks()[0]?.getSettings?.().deviceId;
      const next = nextCamera(devices, current);
      if (!next) return;
      for (const t of stream.getTracks()) t.stop();
      try {
        stream = await open(next);
        choice = next;
        saveCamera(choice);
      } catch (e) {
        stream = await open(choice);
        await attach();
        throw e;
      }
      if (stopped) {
        for (const t of stream.getTracks()) t.stop();
        return;
      }
      await attach();
    },
  };
}
