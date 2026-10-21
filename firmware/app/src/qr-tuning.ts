/**
 * The QR scanner's settings and image work, without a camera: which camera
 * (and remembering it), the brightness and zoom ranges, the decode passes
 * tried on each frame, and the software contrast used when the camera
 * cannot lower its own exposure.
 *
 * Why it exists: a unified address on the board's 240x240 screen is a
 * dense code (version 10 or so, 57+ modules) on a bright, small, glowing
 * surface. Small codes read; that one did not (tablet test, 2026-10-20).
 * What helps, in order: the camera's full resolution on the middle of the
 * picture (where the code is), less exposure (a backlit screen burns the
 * modules into the white), zoom, and other binarizers.
 *
 * No DOM: tested under Node (test/companion-flow.test.ts).
 */

/** Where the chosen camera is kept: "id:<deviceId>" or "facing:<mode>". */
export const CAMERA_KEY = "leek.qr.camera";

type Store = Pick<Storage, "getItem" | "setItem">;

function store(s?: Store): Store | null {
  if (s) return s;
  try { return globalThis.localStorage ?? null; } catch { return null; }
}

/** The camera to open: a device id, or a facing mode when ids are unknown. */
export type CameraChoice = { deviceId: string } | { facing: "environment" | "user" };

export function loadCamera(s?: Store): CameraChoice {
  try {
    const v = store(s)?.getItem(CAMERA_KEY) ?? "";
    if (v.startsWith("id:") && v.length > 3 && v.length < 300) return { deviceId: v.slice(3) };
    if (v === "facing:user") return { facing: "user" };
  } catch { /* the rear camera */ }
  return { facing: "environment" };
}

export function saveCamera(c: CameraChoice, s?: Store): void {
  try {
    store(s)?.setItem(CAMERA_KEY, "deviceId" in c ? `id:${c.deviceId}` : `facing:${c.facing}`);
  } catch { /* remembered for this scan only */ }
}

/** The getUserMedia video constraints for a choice: full HD asked, not demanded. */
export function videoConstraints(c: CameraChoice): MediaTrackConstraints {
  const size = { width: { ideal: 1920 }, height: { ideal: 1080 } };
  return "deviceId" in c
    ? { deviceId: { exact: c.deviceId }, ...size }
    : { facingMode: c.facing, ...size };
}

/**
 * The camera after `current`: the next device id when the cameras are
 * known (their ids are only listed once the camera was allowed), else the
 * other facing mode.
 */
export function nextCamera(devices: { deviceId: string }[], current: CameraChoice, currentDeviceId?: string): CameraChoice {
  const ids = devices.map((d) => d.deviceId).filter((id) => id !== "");
  if (ids.length >= 2) {
    const now = currentDeviceId ?? ("deviceId" in current ? current.deviceId : "");
    const i = ids.indexOf(now);
    return { deviceId: ids[(i + 1) % ids.length]! };
  }
  const facing = "facing" in current ? current.facing : "environment";
  return { facing: facing === "environment" ? "user" : "environment" };
}

/** A slider's range. */
export interface Range { min: number; max: number; step: number; value: number }

/** A camera capability ({min,max,step}) as a slider, or null when absent or flat. */
export function capabilityRange(cap: unknown, value: unknown): Range | null {
  if (typeof cap !== "object" || cap === null) return null;
  const { min, max, step } = cap as { min?: unknown; max?: unknown; step?: unknown };
  if (typeof min !== "number" || typeof max !== "number" || !(max > min)) return null;
  const st = typeof step === "number" && step > 0 ? step : (max - min) / 20;
  const v = typeof value === "number" ? value : min;
  return { min, max, step: st, value: clampToRange(v, { min, max, step: st }) };
}

/** `v` inside the range, on its step. */
export function clampToRange(v: number, r: { min: number; max: number; step: number }): number {
  const c = Math.min(r.max, Math.max(r.min, v));
  const snapped = r.min + Math.round((c - r.min) / r.step) * r.step;
  return Math.min(r.max, Math.max(r.min, Number(snapped.toFixed(6))));
}

/**
 * The software brightness slider, for a camera without exposure control:
 * 0 is the picture as it comes, -3 the darkest (see `enhance`).
 */
export const SOFTWARE_BRIGHTNESS: Range = { min: -3, max: 0, step: 1, value: 0 };

/** A square in the middle of a w x h frame, `fraction` of its short side. */
export function centerCrop(w: number, h: number, fraction = 0.6): { sx: number; sy: number; size: number } {
  const size = Math.max(1, Math.round(Math.min(w, h) * Math.min(1, Math.max(0.1, fraction))));
  return { sx: Math.floor((w - size) / 2), sy: Math.floor((h - size) / 2), size };
}

/** One way of reading a frame. */
export interface DecodePass {
  /** The whole frame (scaled to fit `maxEdge`) or the middle square at full resolution. */
  region: "full" | "center";
  binarizer: "LocalAverage" | "GlobalHistogram";
  /** Run `enhance` on the pixels first. */
  contrast: boolean;
}

/**
 * The passes for frame number `n`: the middle at full resolution every
 * frame (where a code held in front of the camera is), plus one more that
 * rotates, so a frame costs two decodes at most: the whole frame, the
 * middle with software contrast, the middle with the other binarizer.
 * With the software brightness lowered, every pass gets the contrast.
 */
export function decodePasses(n: number, softwareDarken = 0): DecodePass[] {
  const always = softwareDarken < 0;
  const extra: DecodePass[] = [
    { region: "full", binarizer: "LocalAverage", contrast: always },
    { region: "center", binarizer: "LocalAverage", contrast: true },
    { region: "center", binarizer: "GlobalHistogram", contrast: always },
  ];
  return [
    { region: "center", binarizer: "LocalAverage", contrast: always },
    extra[((n % extra.length) + extra.length) % extra.length]!,
  ];
}

/**
 * Grey, stretched and darkened, in place, for a code on a glowing screen:
 * the 2nd to 98th percentile of luminance is stretched to the full range
 * (a washed-out picture gets its contrast back), then a gamma above 1
 * pulls the near-whites down so the modules that the glow merged with the
 * white separate again. `darken` is 0 (stretch only) to -3 (strongest).
 * RGBA pixels; alpha is left alone.
 */
export function enhance(px: Uint8ClampedArray, darken = 0): void {
  const n = px.length >> 2;
  if (n === 0) return;
  const hist = new Uint32Array(256);
  for (let i = 0; i < n; i++) {
    const o = i << 2;
    const y = (px[o]! * 77 + px[o + 1]! * 150 + px[o + 2]! * 29) >> 8;
    px[o] = y;
    hist[y]!++;
  }
  const lowCount = n * 0.02;
  const highCount = n * 0.98;
  /* lo: the darkest level past the darkest 2 %; hi: the level where 98 %
   * of the pixels are reached. */
  let acc = 0;
  let lo = -1;
  let hi = -1;
  for (let v = 0; v < 256; v++) {
    acc += hist[v]!;
    if (lo < 0 && acc > lowCount) lo = v;
    if (hi < 0 && acc >= highCount) hi = v;
  }
  if (lo < 0) lo = 0;
  if (hi < 0) hi = 255;
  if (hi <= lo) hi = Math.min(255, lo + 1);
  const gamma = 1 + Math.min(3, Math.max(0, -darken)) * 0.6;
  const lut = new Uint8ClampedArray(256);
  for (let v = 0; v < 256; v++) {
    const t = Math.min(1, Math.max(0, (v - lo) / (hi - lo)));
    lut[v] = Math.round(255 * Math.pow(t, gamma));
  }
  for (let i = 0; i < n; i++) {
    const o = i << 2;
    const g = lut[px[o]!]!;
    px[o] = g;
    px[o + 1] = g;
    px[o + 2] = g;
  }
}
