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
 */

import { prepareZXingModule, readBarcodes } from "zxing-wasm/reader";
/* Bundled, not fetched: zxing-wasm loads its .wasm from a CDN by default,
 * which the CSP would refuse anyway. Vite turns this into a local asset. */
import zxingWasmUrl from "zxing-wasm/reader/zxing_reader.wasm?url";

let prepared: Promise<unknown> | null = null;
function prepare(): Promise<unknown> {
  prepared ??= prepareZXingModule({ overrides: { locateFile: () => zxingWasmUrl }, fireImmediately: true });
  return prepared;
}

export interface QrScan {
  /** Stops the camera and releases it. Safe to call twice. */
  stop(): void;
}

/** Whether this webview can reach a camera at all. */
export function qrScanningAvailable(): boolean {
  return typeof navigator?.mediaDevices?.getUserMedia === "function";
}

/** Longest edge handed to the decoder. */
const DECODE_MAX_EDGE = 1920;
/** Shortest gap between two decode attempts; the loop paces itself. */
const SCAN_INTERVAL_MS = 120;

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
  const stream = await navigator.mediaDevices.getUserMedia({
    video: { facingMode: "environment", width: { ideal: 1920 }, height: { ideal: 1080 } },
    audio: false,
  });
  let stopped = false;
  let timer: ReturnType<typeof setTimeout> | null = null;
  const stop = (): void => {
    if (stopped) return;
    stopped = true;
    if (timer) clearTimeout(timer);
    for (const t of stream.getTracks()) t.stop();
    video.srcObject = null;
  };
  /* A phone held at a screen or a sheet of paper: continuous autofocus
   * where the camera offers it (ignored where it does not). */
  try {
    await stream.getVideoTracks()[0]?.applyConstraints?.({
      advanced: [{ focusMode: "continuous" } as unknown as MediaTrackConstraintSet],
    });
  } catch { /* the camera keeps its own focus */ }

  const canvas = document.createElement("canvas");
  const ctx = canvas.getContext("2d", { willReadFrequently: true });
  try {
    if (!ctx) throw new Error("This window cannot open a canvas to read the camera.");
    video.srcObject = stream;
    video.setAttribute("playsinline", "");
    video.muted = true;
    await video.play().catch(() => undefined);
    await prepare();
  } catch (e) {
    stop();
    throw e;
  }

  let failures = 0;
  const tick = async (): Promise<void> => {
    if (stopped) return;
    try {
      const w = video.videoWidth;
      const h = video.videoHeight;
      if (w > 0 && h > 0) {
        const scale = Math.min(1, DECODE_MAX_EDGE / Math.max(w, h));
        const dw = Math.round(w * scale);
        const dh = Math.round(h * scale);
        if (canvas.width !== dw || canvas.height !== dh) { canvas.width = dw; canvas.height = dh; }
        ctx!.drawImage(video, 0, 0, dw, dh);
        const found = await readBarcodes(ctx!.getImageData(0, 0, dw, dh), { formats: ["QRCode"], tryHarder: true, tryInvert: true });
        failures = 0;
        for (const f of found) {
          const v = accept(f.text.trim());
          if (v !== undefined) {
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
  return { stop };
}
