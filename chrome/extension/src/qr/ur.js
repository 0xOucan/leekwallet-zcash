// host/leek-ur in the page: chrome/ur (wasm-bindgen, packaged as ur/ in
// dist), the UR implementation zecz, the desktop app and Android use. The
// page never encodes or assembles a UR itself; it draws the frames this
// gives it and feeds it what the camera reads.
//
// Loaded in the page, not the engine's worker: the worker runs one call at
// a time and a sync holds it for minutes, while the loop needs a frame
// every few hundred milliseconds. The .zsig still goes to the engine.

let loaded = null;

/** The module, initialised once. Tests pass their own (initSync'd). */
export function loadUr(override = null) {
  if (override) return Promise.resolve(override);
  loaded ??= (async () => {
    const url = new URL('../../ur/leek_chrome_ur.js', import.meta.url);
    const m = await import(url.href);
    await m.default({ module_or_path: new URL('../../ur/leek_chrome_ur_bg.wasm', import.meta.url) });
    return m;
  })();
  return loaded;
}

export const hex = (bytes) => Array.from(bytes, (b) => b.toString(16).padStart(2, '0')).join('');

export function fromHex(s) {
  const t = String(s).trim();
  if (t.length % 2 || /[^0-9a-f]/i.test(t)) throw new Error('not hex');
  const out = new Uint8Array(t.length / 2);
  for (let i = 0; i < out.length; i++) out[i] = parseInt(t.slice(2 * i, 2 * i + 2), 16);
  return out;
}
