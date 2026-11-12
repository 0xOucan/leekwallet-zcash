// The QR screens, drawn over the popup opened as a tab (popup.html?tab):
// Sign with QR (the ur:leek-pczt loop, then the webcam for the board's
// ur:leek-zsig), Import from the board's QR (its View key), and an address
// shown for the board's Check address. Never pairing: the board pairs over
// USB and Bluetooth, with its six digits.
//
// An overlay on a black page, outside #root: the popup redraws #root on
// every sync batch, and the loop must not flicker with it. The state
// machines are src/qr/sign.js and src/qr/accounts.js; this file is the DOM.

import { QrAccountImport, accountsCaption } from './accounts.js';
import { drawQr, qrAvail, qrVersionFor } from './qr-code.js';
import { QrSigner, frameCaption, scanCaption } from './sign.js';
import { cameraError, scanQr, scanningAvailable } from './scan.js';
import { fromHex, hex, loadUr } from './ur.js';

/** The board's own line for an address QR (leek-ur ADDRESS_CHECK_HINT). */
export const ADDRESS_CHECK_HINT = 'On the board: Main menu → Check address, then scan this code.';

let qrcode = null;
async function qrGen() {
  qrcode ??= (await import('../vendor/qrcode-generator/qrcode.mjs')).default;
  return qrcode;
}

function h(tag, attrs = {}, ...children) {
  const el = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) {
    if (v === undefined || v === null || v === false) continue;
    if (k.startsWith('on')) el.addEventListener(k.slice(2), v);
    else if (k === 'class') el.className = v;
    else if (v === true) el.setAttribute(k, '');
    else el.setAttribute(k, String(v));
  }
  for (const c of children.flat(Infinity)) {
    if (c === null || c === undefined || c === false) continue;
    el.append(c instanceof Node ? c : document.createTextNode(String(c)));
  }
  return el;
}

/** The overlay, one at a time. Returns `{ el, body, close }`; `cleanup` runs on close. */
function overlay(title, onClose) {
  document.getElementById('qr-overlay')?.closeOverlay?.();
  const body = h('div', { class: 'qr-body' });
  const cleanups = [];
  let closed = false;
  const close = () => {
    if (closed) return;
    closed = true;
    if (document.fullscreenElement) void document.exitFullscreen().catch(() => {});
    for (const f of cleanups) f();
    el.remove();
    document.body.classList.remove('qr-open');
    onClose?.();
  };
  const el = h(
    'div',
    { id: 'qr-overlay', class: 'qr-overlay', role: 'dialog', 'aria-label': title },
    h('div', { class: 'qr-head' }, h('h2', {}, title), h('button', { class: 'link', id: 'qr-close', onclick: () => close() }, 'Close')),
    body,
  );
  el.closeOverlay = close;
  document.body.append(el);
  document.body.classList.add('qr-open');
  return { el, body, close, cleanup: (f) => cleanups.push(f) };
}

/** Room for the code: the window, less the captions and buttons under it. */
function avail(reserve) {
  return qrAvail(window.innerWidth - 16, window.innerHeight, reserve);
}

// ------------------------------------------------------- Sign with QR

/**
 * Sign `proposal` (the engine's proposeSend/proposeShield answer) over QR.
 * `engine` is the EngineClient; `onDone(result)` gets the engine's finish
 * answer (txid, broadcast) once the send is stored and broadcast.
 */
export async function openQrSign({ engine, network, proposal, confirmMainnet = false, onDone, onClose }) {
  if (network === 'main' && confirmMainnet !== true) throw new Error('This moves real ZEC on mainnet: tick the confirmation first.');
  const ur = await loadUr();
  const gen = await qrGen();
  const hints = ur.displayHints();
  const tips = ur.largeScreenTips();
  const view = fromHex(proposal.signerView);
  const q = new QrSigner(ur, { hints: { fragmentLen: hints.fragmentLen, frameMs: hints.frameMs }, versionFor: (n) => qrVersionFor(n) });
  const quiet = hints.quietZoneModules;
  let scan = null;
  let status = '';
  let result = null;
  let alive = true;

  const { el, body, close, cleanup } = overlay('Sign with QR', () => {
    alive = false;
    scan?.stop();
    if (q.phase === 'showing' || q.phase === 'scanning') q.cancel();
    onClose?.(result);
  });
  el.classList.add('qr-black');

  const canvas = h('canvas', { id: 'qr-canvas', 'aria-label': 'the transaction for the board, as an animated QR code' });
  const stage = h('div', { class: 'qr-stage' }, canvas);
  const caption = h('p', { class: 'qr-caption mono', id: 'qr-frame' });
  const video = h('video', { id: 'qr-video', class: 'qr-video', playsinline: true, muted: true });
  const line = h('p', { class: 'qr-line', id: 'qr-status', 'aria-live': 'polite' });
  const controls = h('div', { class: 'qr-controls' });
  const help = h('div', { class: 'qr-help' });

  const fullscreen = () => {
    if (document.fullscreenElement) void document.exitFullscreen();
    else void el.requestFullscreen?.().catch(() => {});
  };

  const stopCamera = () => {
    scan?.stop();
    scan = null;
  };

  async function finish(zsig) {
    stopCamera();
    status = `Read the board’s signature (${zsig.length} B). Checking each signature, then proving…`;
    draw();
    try {
      if (engine.host?.busy === 'sync') {
        status = 'Waiting for the sync to finish its batch, then checking the signatures and proving…';
        draw();
      }
      const fin = await engine.call('finish', { signerView: proposal.signerView, zsig: hex(zsig), broadcast: true });
      result = { txid: fin.txid, broadcast: fin.broadcast, size: fin.size, proveMs: fin.proveMs, signatures: fin.signatures, via: 'qr' };
      q.finished();
      status = '';
      onDone?.(result);
    } catch (e) {
      q.fail(e);
    }
  }

  async function startScan() {
    if (!scanningAvailable()) {
      q.fail(new Error('This page cannot reach a camera, so the board’s reply cannot be read here. Sign over USB or Bluetooth instead.'));
      return;
    }
    q.startScan();
    try {
      scan = await scanQr(
        video,
        (t) => {
          // Every code goes to the decoder; the scan is stopped from here.
          const zsig = q.feed(t);
          if (zsig && alive) void finish(zsig);
          return undefined;
        },
        () => {},
        (m) => {
          stopCamera();
          q.fail(new Error(m));
        },
        { animated: true },
      );
      if (!alive || q.phase !== 'scanning') stopCamera();
      draw();
    } catch (e) {
      stopCamera();
      q.fail(new Error(cameraError(e)));
    }
  }

  let lastPhase = null;
  function draw() {
    if (!alive) return;
    const f = q.frame();
    if (f) {
      drawQr(canvas, gen, f.text, { version: f.version, avail: avail(document.fullscreenElement ? 72 : 260), dpr: window.devicePixelRatio || 1, quiet });
      caption.textContent = frameCaption(f);
    }
    if (q.phase === 'scanning') line.textContent = scanCaption(q.progress(), q.note);
    else if (q.phase === 'finishing') line.textContent = status || 'Checking the signatures and proving…';
    else if (q.phase === 'failed') line.textContent = q.error ?? 'QR signing failed.';
    else if (q.phase === 'done') line.textContent = '';
    else line.textContent = status;
    line.className = `qr-line${q.phase === 'failed' ? ' bad' : ''}`;
    if (q.phase === lastPhase) return;
    lastPhase = q.phase;
    layout();
  }

  function layout() {
    const p = q.phase;
    stage.hidden = p !== 'showing';
    caption.hidden = p !== 'showing';
    video.hidden = p !== 'scanning';
    const presetSelect = h(
      'select',
      { id: 'qr-preset', 'aria-label': 'speed', onchange: (e) => q.setPreset(e.target.value) },
      Object.entries(q.presets).map(([k, v]) => h('option', { value: k, selected: k === q.preset }, v.label)),
    );
    const buttons = [];
    if (p === 'showing') {
      buttons.push(
        h('button', { class: 'primary', id: 'qr-scan-reply', onclick: () => void startScan() }, 'Scan the board’s signature'),
        presetSelect,
        h('button', { id: 'qr-fullscreen', onclick: fullscreen }, 'Full screen'),
        h('button', { id: 'qr-cancel', onclick: () => close() }, 'Cancel'),
      );
    } else if (p === 'scanning') {
      buttons.push(
        h('button', { id: 'qr-show-again', onclick: () => (stopCamera(), q.showAgain()) }, 'Show the transaction again'),
        scan && scan.cameras() > 1 ? h('button', { id: 'qr-switch-camera', onclick: () => void scan.switchCamera().catch((e) => q.fail(new Error(cameraError(e)))) }, 'Switch camera') : null,
        h('button', { id: 'qr-cancel', onclick: () => close() }, 'Cancel'),
      );
    } else if (p === 'done') {
      buttons.push(h('button', { class: 'primary', id: 'qr-done', onclick: () => close() }, 'Done'));
    } else if (p === 'failed') {
      buttons.push(
        h('button', { id: 'qr-retry', onclick: () => q.show(view) }, 'Show the transaction again'),
        h('button', { id: 'qr-cancel', onclick: () => close() }, 'Close'),
      );
    }
    controls.replaceChildren(...buttons.filter(Boolean));
    if (p === 'showing') {
      help.replaceChildren(
        h('p', {}, 'On the board: Main menu → Scan, and point its camera at this code (the board opens at 2x for a laptop screen). It shows the same review as here: compare, then ACCEPT on its last page signs. The board then shows its signature as a QR code: press “Scan the board’s signature”.'),
        h('p', { class: 'muted' }, tips.join(' '), ' If the board finds the code but does not read it: Smaller codes, or Safe for board firmware from before 2026-11-11.'),
      );
    } else if (p === 'scanning') {
      help.replaceChildren(h('p', {}, 'Hold the board’s screen in front of the webcam, about 20–30 cm away, until every part is read. Nothing is broadcast until each signature is checked against this transaction.'));
    } else if (p === 'done' && result) {
      help.replaceChildren(
        h('p', { class: 'notice', id: 'qr-txid' }, `Sent (signed over QR)${result.broadcast ? '' : ', not broadcast'}: `, h('code', {}, result.txid)),
        h('p', { class: 'muted' }, 'It shows as pending until it is mined.'),
      );
    } else {
      help.replaceChildren();
    }
  }

  body.append(stage, caption, video, line, controls, help);
  q.onChange = draw;
  const onResize = () => q.phase === 'showing' && draw();
  window.addEventListener('resize', onResize);
  document.addEventListener('fullscreenchange', onResize);
  cleanup(() => {
    window.removeEventListener('resize', onResize);
    document.removeEventListener('fullscreenchange', onResize);
  });
  q.show(view);
  return { signer: q, close };
}

// ------------------------------------------- Import from the board's QR

/**
 * Read the board's View key (ur:leek-zcash-accounts) with the webcam and
 * import it as a device account of `network`. `onDone(r)` gets the
 * engine's importAccount answer plus `seedFingerprint` and `birthday`.
 */
export async function openQrImport({ engine, network, onDone, onClose }) {
  const ur = await loadUr();
  const q = new QrAccountImport(ur);
  let scan = null;
  let alive = true;
  const { body, close } = overlay('Import from the board’s QR', () => {
    alive = false;
    scan?.stop();
    onClose?.();
  });
  const video = h('video', { id: 'qr-video', class: 'qr-video', playsinline: true, muted: true });
  const line = h('p', { class: 'qr-line', id: 'qr-status', 'aria-live': 'polite' });
  const panel = h('div', { class: 'qr-help' });
  const controls = h('div', { class: 'qr-controls' });
  const birthday = h('input', { id: 'qr-birthday', type: 'text', inputmode: 'numeric', placeholder: 'empty: a new seed (near the chain tip)', spellcheck: 'false' });

  const stopCamera = () => {
    scan?.stop();
    scan = null;
  };

  async function start() {
    q.start(network);
    if (!scanningAvailable()) return q.fail(new Error('This page cannot reach a camera.'));
    try {
      scan = await scanQr(video, (t) => (q.feed(t) ? true : undefined), () => stopCamera(), (m) => (stopCamera(), q.fail(new Error(m))), { animated: true });
      if (!alive || q.phase !== 'scanning') stopCamera();
    } catch (e) {
      stopCamera();
      q.fail(new Error(cameraError(e)));
    }
  }

  async function doImport() {
    try {
      const fp = q.record.seedFingerprint;
      const r = await q.importFound((m, p) => engine.call(m, p), birthday.value);
      await onDone?.({ ...r, seedFingerprint: fp });
      close();
    } catch {
      /* shown from q.error */
    }
  }

  let lastPhase = null;
  function draw() {
    if (!alive) return;
    video.hidden = q.phase !== 'scanning';
    line.className = `qr-line${q.error && q.phase !== 'scanning' ? ' bad' : ''}`;
    line.textContent = q.phase === 'scanning' ? accountsCaption(q) : q.phase === 'importing' ? 'Importing…' : q.error ?? '';
    if (q.phase === lastPhase) return;
    lastPhase = q.phase;
    const a = q.account();
    const r = q.record;
    const nets = { main: 'Mainnet', test: 'Testnet' };
    if (q.phase === 'scanning') {
      panel.replaceChildren(
        h('p', {}, 'On the board: Main menu → View key, and confirm there. It shows its account as an animated QR code: hold it in front of the webcam until every part is read.'),
        h('p', { class: 'muted' }, 'One way only: the viewing key comes in, nothing goes to the board, and its spending keys stay on it. This is not pairing: USB and Bluetooth keep their six-digit code.'),
      );
      controls.replaceChildren(h('button', { id: 'qr-cancel', onclick: () => close() }, 'Cancel'));
    } else if ((q.phase === 'found' || q.phase === 'importing') && r && a) {
      panel.replaceChildren(
        h(
          'table',
          { id: 'qr-record' },
          h('tr', {}, h('td', {}, 'Network'), h('td', {}, nets[r.network])),
          h('tr', {}, h('td', {}, 'Account'), h('td', {}, String(a.index))),
          h('tr', {}, h('td', {}, 'Seed fingerprint'), h('td', { class: 'mono', id: 'qr-zfp' }, `ZFP ${r.zfp}`)),
          a.name ? h('tr', {}, h('td', {}, 'Label'), h('td', {}, a.name)) : null,
          a.birthday != null ? h('tr', {}, h('td', {}, 'Birthday'), h('td', {}, a.birthday.toLocaleString())) : null,
          r.accounts.length > 1 ? h('tr', {}, h('td', {}, 'Note'), h('td', {}, `the code holds ${r.accounts.length} accounts; account ${a.index} is imported`)) : null,
        ),
        a.birthday == null
          ? h('label', {}, h('span', {}, 'Birthday height: the block the seed was created at. Empty for a new seed.'), birthday)
          : null,
        h('p', { class: 'muted' }, 'Check that the board shows the same ZFP. Imported this way, the wallet is shown here without the board connected (it has no cable to show it with), and signs over QR, or over USB or Bluetooth once connected. Its address is derived here from the viewing key; check it on the board with Check address.'),
      );
      controls.replaceChildren(
        h('button', { class: 'primary', id: 'qr-import', onclick: () => void doImport(), disabled: q.phase === 'importing' }, 'Import'),
        h('button', { id: 'qr-cancel', onclick: () => close(), disabled: q.phase === 'importing' }, 'Cancel'),
      );
    } else if (q.phase === 'failed') {
      panel.replaceChildren();
      controls.replaceChildren(h('button', { id: 'qr-retry', onclick: () => void start() }, 'Scan again'), h('button', { id: 'qr-cancel', onclick: () => close() }, 'Close'));
    }
  }
  q.onChange = draw;
  body.append(video, line, panel, controls);
  await start();
  return { importer: q, close };
}

// ------------------------------------------ an address for Check address

/** The address as a plain QR (byte mode, its case kept), for the board's Check address. */
export async function openAddressQr({ address, onClose }) {
  const gen = await qrGen();
  const { el, body, cleanup } = overlay('Check on the board', onClose);
  el.classList.add('qr-black');
  const canvas = h('canvas', { id: 'qr-address' });
  body.append(
    h('div', { class: 'qr-stage' }, canvas),
    h('div', { class: 'qr-help' }, h('p', {}, ADDRESS_CHECK_HINT), h('p', { class: 'muted mono qr-wrap' }, address), h('p', { class: 'muted' }, 'The board says whether the address is its own, with the account and the diversifier index.')),
  );
  const draw = () => drawQr(canvas, gen, address, { avail: Math.min(avail(260), 520), dpr: window.devicePixelRatio || 1, quiet: 2, mode: 'Byte' });
  draw();
  window.addEventListener('resize', draw);
  cleanup(() => window.removeEventListener('resize', draw));
}
