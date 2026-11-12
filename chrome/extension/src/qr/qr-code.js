// Drawing a QR code for the board's camera, by the display rules of
// host/leek-ur/src/display.rs (docs/zcash/FORMATS.md, "Showing
// ur:leek-pczt to the board"), as the desktop app's src/qr-sign.ts draws
// it: error correction L, alphanumeric mode (every frame is uppercase), one
// QR version for the whole loop (its longest frame), black modules on
// white, a whole number of device pixels per module, a 2-module white
// quiet zone on a black page.
//
// The encoder is qrcode-generator (MIT, vendored at build time from the
// pinned npm package, as the desktop app pins it). No DOM needed but the
// canvas passed in: the maths is tested under Node.

/** QR alphanumeric capacity at error correction L, versions 1-40. */
const ALNUM_L = [
  25, 47, 77, 114, 154, 195, 224, 279, 335, 395, 468, 535, 619, 667, 758, 854, 938, 1046, 1153, 1249,
  1352, 1460, 1588, 1704, 1853, 1990, 2132, 2223, 2369, 2520, 2677, 2840, 3009, 3183, 3351, 3537, 3729, 3927, 4087, 4296,
];

/** QR byte-mode capacity at error correction L, versions 1-40 (an address QR). */
const BYTE_L = [
  17, 32, 53, 78, 106, 134, 154, 192, 230, 271, 321, 367, 425, 458, 520, 586, 644, 718, 792, 858,
  929, 1003, 1091, 1171, 1273, 1367, 1465, 1528, 1628, 1732, 1840, 1952, 2068, 2188, 2303, 2431, 2563, 2699, 2809, 2953,
];

/** The smallest QR version (error correction L) that holds `chars` characters in `mode`. */
export function qrVersionFor(chars, mode = 'Alphanumeric') {
  const table = mode === 'Byte' ? BYTE_L : ALNUM_L;
  const i = table.findIndex((c) => c >= chars);
  if (i < 0) throw new Error(`a QR code of ${chars} characters is too long`);
  return i + 1;
}

/** Modules on a side of a QR code of `version`. */
export const modulesFor = (version) => 17 + 4 * version;

/** Pixels per module and the drawn side for `modules` (quiet zone included) in `avail` px. */
export function moduleScale(modules, avail) {
  const px = Math.max(1, Math.floor(avail / modules));
  return { px, side: px * modules };
}

/**
 * Room for the code: the smaller side of the page, less `reserve` px of
 * height for the captions under it.
 */
export function qrAvail(width, height, reserve = 0) {
  return Math.max(1, Math.floor(Math.min(width, height - Math.max(0, reserve))));
}

/**
 * Draw `text` on `canvas`: version `version` (0: the smallest that holds
 * it), error correction L, `mode` "Alphanumeric" (a UR frame) or "Byte"
 * (a plain address, whose Base58 or Bech32m case must stay), a
 * `quiet`-module white quiet zone, in `avail` CSS px times `dpr` (so a
 * module is whole device pixels). `gen` is qrcode-generator's `qrcode`.
 */
export function drawQr(canvas, gen, text, { version = 0, avail, dpr = 1, quiet = 2, mode = 'Alphanumeric' }) {
  const qr = gen(version, 'L');
  qr.addData(text, mode);
  qr.make();
  const n = qr.getModuleCount();
  const { px, side } = moduleScale(n + quiet * 2, Math.floor(avail * dpr));
  if (canvas.width !== side) {
    canvas.width = side;
    canvas.height = side;
  }
  if (canvas.style) {
    canvas.style.width = `${side / dpr}px`;
    canvas.style.height = `${side / dpr}px`;
  }
  const ctx = canvas.getContext('2d');
  if (!ctx) return n;
  ctx.imageSmoothingEnabled = false;
  ctx.fillStyle = '#ffffff';
  ctx.fillRect(0, 0, side, side);
  ctx.fillStyle = '#000000';
  for (let y = 0; y < n; y++) {
    for (let x = 0; x < n; x++) {
      if (qr.isDark(y, x)) ctx.fillRect((x + quiet) * px, (y + quiet) * px, px, px);
    }
  }
  return n;
}

/**
 * The same code as RGBA pixels, `px` per module (tests, and the e2e's
 * stand-in camera): what drawQr puts on a canvas.
 */
export function qrPixels(gen, text, { version = 0, px = 4, quiet = 2, mode = 'Alphanumeric' } = {}) {
  const qr = gen(version, 'L');
  qr.addData(text, mode);
  qr.make();
  const n = qr.getModuleCount();
  const side = (n + 2 * quiet) * px;
  const data = new Uint8ClampedArray(side * side * 4).fill(255);
  for (let y = 0; y < n; y++) {
    for (let x = 0; x < n; x++) {
      if (!qr.isDark(y, x)) continue;
      for (let dy = 0; dy < px; dy++) {
        const row = ((y + quiet) * px + dy) * side;
        for (let dx = 0; dx < px; dx++) {
          const o = (row + (x + quiet) * px + dx) * 4;
          data[o] = data[o + 1] = data[o + 2] = 0;
        }
      }
    }
  }
  return { data, width: side, height: side, modules: n };
}
