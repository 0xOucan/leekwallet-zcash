/**
 * Camera QR scanning for the Send form: a Zcash address or a ZIP 321
 * `zcash:` payment request (parsed by packages/core/src/zip321.ts). Zcash
 * only, and nothing else: there is no QR flow for pairing or signing (the
 * board is reached over USB and Bluetooth only).
 *
 * Decoding is the bundled `zxing-wasm` reader (MIT, WebAssembly, shipped
 * inside the app and served from 'self'; nothing is fetched). The same
 * decoder this app used before for its EVM scanner, chosen then over the
 * alternatives after measuring them, and kept for the same reasons:
 *
 * - `BarcodeDetector` does not exist in WebKitGTK (the Linux desktop), and
 *   on Android Chromium backs it with Google Play Services, which crashed
 *   the app outright without extra manifest wiring.
 * - jsQR could not read dense codes off a screen; ZXing could. A unified
 *   address is a dense code (about 140 characters, more with a memo).
 * - Tauri's barcode-scanner plugin is mobile only: the desktop would need a
 *   second scanner, and it adds a native camera activity and its Kotlin to
 *   the APK. One webview code path on every platform is less to trust.
 *
 * The costs: about a megabyte of WebAssembly, and `wasm-unsafe-eval` in the
 * CSP's script-src (compiling WebAssembly; it does not allow `eval` of
 * JavaScript). The camera needs: on Android the CAMERA permission in the
 * manifest (Tauri's webview client asks the user), on Linux the
 * WebKitGTK media-stream grant in src-tauri/src/lib.rs.
 *
 * Frames go from the <video> to an offscreen canvas and into the decoder;
 * nothing is stored or sent anywhere, and the camera is released on stop.
 *
 * For dense codes on bright screens (a unified address on the board's
 * 240x240 display, 2026-10-20): the camera at 1920x1080 when it has it,
 * continuous autofocus, the middle of the picture decoded at full
 * resolution every frame, other binarizers and a software contrast pass in
 * rotation (src/qr-tuning.ts), and three controls: brightness (the
 * camera's exposure compensation, or the software contrast when it has
 * none), zoom when the camera offers it, and switching cameras. The camera
 * chosen is remembered (leek.qr.camera).
 */

import { prepareZXingModule, readBarcodes } from "zxing-wasm/reader";
/* Bundled, not fetched: zxing-wasm loads its .wasm from a CDN by default,
 * which the CSP would refuse anyway. Vite turns this into a local asset. */
import zxingWasmUrl from "zxing-wasm/reader/zxing_reader.wasm?url";
import {
  capabilityRange, centerCrop, clampToRange, decodePasses, enhance, loadCamera, nextCamera, saveCamera,
  videoConstraints, SOFTWARE_BRIGHTNESS, type CameraChoice, type Range,
} from "./qr-tuning.ts";

let prepared: Promise<unknown> | null = null;
function prepare(): Promise<unknown> {
  prepared ??= prepareZXingModule({ overrides: { locateFile: () => zxingWasmUrl }, fireImmediately: true });
  return prepared;
}

export interface QrScan {
  /** Stops the camera and releases it. Safe to call twice. */
  stop(): void;
  /** The brightness slider: the camera's exposure compensation, or software. */
  brightness(): { range: Range; hardware: boolean };
  setBrightness(v: number): void;
  /** The zoom slider, or null when this camera has no zoom. */
  zoom(): Range | null;
  setZoom(v: number): void;
  /** How many cameras there are to switch between (2 when unknown but likely). */
  cameras(): number;
  /** The next camera, remembered for the next scan. */
  switchCamera(): Promise<void>;
}

/** Whether this webview can reach a camera at all. */
export function qrScanningAvailable(): boolean {
  return typeof navigator?.mediaDevices?.getUserMedia === "function";
}

/** Longest edge of the whole frame handed to the decoder. */
const DECODE_MAX_EDGE = 1920;
/** Shortest gap between two decode attempts; the loop paces itself. */
const SCAN_INTERVAL_MS = 120;

type Caps = MediaTrackCapabilities & {
  exposureCompensation?: { min: number; max: number; step?: number };
  exposureMode?: string[];
  zoom?: { min: number; max: number; step?: number };
  focusMode?: string[];
};
type Settings = MediaTrackSettings & { exposureCompensation?: number; zoom?: number };

async function open(choice: CameraChoice): Promise<MediaStream> {
  try {
    return await navigator.mediaDevices.getUserMedia({ video: videoConstraints(choice), audio: false });
  } catch (e) {
    /* A remembered camera that is gone (another device, a camera removed):
     * the rear one instead, and forget the old choice. */
    const name = (e as { name?: string }).name;
    if ("deviceId" in choice && (name === "OverconstrainedError" || name === "NotFoundError" || name === "NotReadableError")) {
      saveCamera({ facing: "environment" });
      return navigator.mediaDevices.getUserMedia({ video: videoConstraints({ facing: "environment" }), audio: false });
    }
    throw e;
  }
}

/**
 * Scan until `accept` returns something for a decoded text, then stop the
 * camera and hand it to `onResult`. Codes `accept` ignores (an unrelated QR
 * in shot) do not end the scan.
 */
export async function scanQr<T>(
  video: HTMLVideoElement,
  accept: (text: string) => T | undefined,
  onResult: (value: T) => void,
  onError: (message: string) => void,
): Promise<QrScan> {
  if (!qrScanningAvailable()) throw new Error("This window cannot reach a camera. Paste the address instead.");
  void prepare();
  let choice = loadCamera();
  let stream = await open(choice);
  let stopped = false;
  let timer: ReturnType<typeof setTimeout> | null = null;
  let softwareDarken = 0;
  let devices: MediaDeviceInfo[] = [];

  const track = (): MediaStreamTrack | undefined => stream.getVideoTracks()[0];
  const caps = (): Caps => {
    try { return (track()?.getCapabilities?.() ?? {}) as Caps; } catch { return {}; }
  };
  const settings = (): Settings => {
    try { return (track()?.getSettings?.() ?? {}) as Settings; } catch { return {}; }
  };
  const apply = async (c: Record<string, unknown>): Promise<void> => {
    try {
      await track()?.applyConstraints?.({ advanced: [c as MediaTrackConstraintSet] });
    } catch { /* the camera keeps its own setting */ }
  };

  const stop = (): void => {
    if (stopped) return;
    stopped = true;
    if (timer) clearTimeout(timer);
    for (const t of stream.getTracks()) t.stop();
    video.srcObject = null;
  };

  /* A phone held at a screen or a sheet of paper: continuous autofocus
   * where the camera offers it (ignored where it does not). */
  const focus = async (): Promise<void> => {
    if (caps().focusMode?.includes("continuous") ?? true) await apply({ focusMode: "continuous" });
  };

  const attach = async (): Promise<void> => {
    video.srcObject = stream;
    video.setAttribute("playsinline", "");
    video.muted = true;
    await video.play().catch(() => undefined);
    await focus();
    try {
      devices = (await navigator.mediaDevices.enumerateDevices()).filter((d) => d.kind === "videoinput");
    } catch { devices = []; }
  };

  const canvas = document.createElement("canvas");
  const ctx = canvas.getContext("2d", { willReadFrequently: true });
  try {
    if (!ctx) throw new Error("This window cannot open a canvas to read the camera.");
    await attach();
    await prepare();
  } catch (e) {
    stop();
    throw e;
  }

  /** One region of the current frame as pixels, enhanced when asked. */
  const grab = (region: "full" | "center", contrast: boolean): ImageData | null => {
    const w = video.videoWidth;
    const h = video.videoHeight;
    if (!(w > 0 && h > 0)) return null;
    let sx = 0, sy = 0, sw = w, sh = h, dw: number, dh: number;
    if (region === "center") {
      const c = centerCrop(w, h);
      sx = c.sx; sy = c.sy; sw = c.size; sh = c.size;
      dw = c.size; dh = c.size; // full resolution: every pixel of the middle
    } else {
      const scale = Math.min(1, DECODE_MAX_EDGE / Math.max(w, h));
      dw = Math.round(w * scale);
      dh = Math.round(h * scale);
    }
    if (canvas.width !== dw || canvas.height !== dh) { canvas.width = dw; canvas.height = dh; }
    ctx!.drawImage(video, sx, sy, sw, sh, 0, 0, dw, dh);
    const img = ctx!.getImageData(0, 0, dw, dh);
    if (contrast) enhance(img.data, softwareDarken);
    return img;
  };

  let failures = 0;
  let frame = 0;
  const tick = async (): Promise<void> => {
    if (stopped) return;
    try {
      for (const pass of decodePasses(frame++, softwareDarken)) {
        if (stopped) return;
        const img = grab(pass.region, pass.contrast);
        if (!img) break;
        const found = await readBarcodes(img, {
          formats: ["QRCode"], tryHarder: true, tryInvert: true, binarizer: pass.binarizer, maxNumberOfSymbols: 4,
        });
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
      /* One bad frame is not the end of a scan; a decoder failing every
       * frame is. */
      if (++failures >= 20) {
        stop();
        onError(`The QR decoder failed: ${(e as Error).message ?? String(e)}`);
        return;
      }
    }
    if (!stopped) timer = setTimeout(() => void tick(), SCAN_INTERVAL_MS);
  };
  timer = setTimeout(() => void tick(), SCAN_INTERVAL_MS);

  return {
    stop,
    brightness() {
      const hw = capabilityRange(caps().exposureCompensation, settings().exposureCompensation);
      return hw ? { range: hw, hardware: true } : { range: { ...SOFTWARE_BRIGHTNESS, value: softwareDarken }, hardware: false };
    },
    setBrightness(v) {
      const hw = capabilityRange(caps().exposureCompensation, settings().exposureCompensation);
      if (hw) {
        const ev = clampToRange(v, hw);
        void apply({ exposureMode: "continuous", exposureCompensation: ev });
        /* Both: a screen bright enough to need less exposure usually also
         * reads better with the contrast pass. */
        softwareDarken = ev < 0 ? Math.max(-3, Math.round(ev)) : 0;
      } else {
        softwareDarken = clampToRange(v, SOFTWARE_BRIGHTNESS);
      }
    },
    zoom() {
      return capabilityRange(caps().zoom, settings().zoom);
    },
    setZoom(v) {
      const r = capabilityRange(caps().zoom, settings().zoom);
      if (r) void apply({ zoom: clampToRange(v, r) });
    },
    cameras() {
      const known = devices.filter((d) => d.deviceId !== "").length;
      return known > 0 ? known : 2;
    },
    async switchCamera() {
      if (stopped) return;
      const next = nextCamera(devices, choice, settings().deviceId);
      for (const t of stream.getTracks()) t.stop();
      try {
        stream = await open(next);
        choice = next;
      } catch (e) {
        /* The other camera would not open: back to the one that did. */
        stream = await open(choice);
        await attach();
        throw e;
      }
      if (stopped) { for (const t of stream.getTracks()) t.stop(); return; }
      saveCamera(choice);
      softwareDarken = 0;
      await attach();
    },
  };
}
