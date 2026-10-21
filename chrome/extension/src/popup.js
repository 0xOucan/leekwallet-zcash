// The popup: network switch, import a viewing key, sync progress,
// balances per pool, receive address (with rotation), recent history,
// and the board (9.3): connect over USB or Bluetooth, import its account,
// check an address on its screen, send and shield signed on it.
//
// A device account (imported from a board) is shown only while the board
// that holds its seed is connected: its seed fingerprint, read from the
// board on connect, must match the account's. Otherwise the popup shows
// the board section and nothing of the wallet; the synced data stays on
// OPFS, so reconnecting shows it at once. View-only accounts are always
// shown.
//
// It owns nothing: every answer comes from the engine through
// EngineClient, and the few choices it remembers (network, endpoint,
// account) are in chrome.storage.local. Text from the chain (memos,
// addresses) only ever goes into textContent.

import { EngineClient } from './engine-client.js';
import { formatZec, historyEntry } from './format.js';
import { BLE_TAB_WHY, LocalBoard, RemoteBoard, USB_TAB_WHY, bluetoothHelp, grantUsb, inTab, usbPlan } from './board-client.js';
import { passphraseComplaint } from './board/host.js';

const root = document.getElementById('root');
const query = new URLSearchParams(location.search);
if (query.has('tab')) document.body.classList.add('tab');

const NETWORKS = { test: 'Testnet', main: 'Mainnet' };
const DEFAULT_SERVERS = {
  test: 'https://zcash-testnet.chainsafe.dev',
  main: 'https://zcash-mainnet.chainsafe.dev',
};

/** UI state; the wallet state is the engine's. */
const ui = {
  engine: null,
  prefs: { network: 'test', servers: {}, account: {} },
  status: null, // the engine's `open`/`status` answer
  balances: null,
  addresses: null,
  rotated: null,
  history: [],
  sync: null, // last sync event
  syncing: false,
  error: null,
  busy: null, // a label while a call the user started runs
  board: null, // RemoteBoard (USB, offscreen) or LocalBoard (Bluetooth, this tab)
  boardState: { phase: 'disconnected' },
  needUsbGrant: false,
  needBirthday: null, // { tip } when the board has no birthday for this network
  proposal: null, // { kind: 'send'|'shield', ...the engine's proposal }
  confirmMainnet: false,
  signing: null, // progress line while the board signs and the engine finishes
  lastTx: null,
  verified: null, // the address the board confirmed
  shown: null, // the account whose data ui.balances/addresses/history hold
  confirmForget: false,
  quietCancel: false, // a sync stopped by the popup itself, not the user
  switching: null, // 'ble' after "Switch to Bluetooth" ended the USB session
  forgotten: new Set(), // accounts "Forget this board" is deleting: hidden at once
  usbTabOnly: false, // the USB port is held by this tab (the offscreen document cannot see the grant)
  passHint: null, // the outcome of a passphrase sent from here (never the passphrase)
};

// ------------------------------------------------------------- helpers

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

async function loadPrefs() {
  const got = await chrome.storage.local.get({ prefs: ui.prefs });
  ui.prefs = { ...ui.prefs, ...got.prefs };
}

async function savePrefs() {
  await chrome.storage.local.set({ prefs: ui.prefs });
}

const net = () => ui.prefs.network;
const server = () => ui.prefs.servers[net()] || DEFAULT_SERVERS[net()];
/** The fingerprint of the seed on the connected board, or null. */
const boardFingerprint = () => (ui.boardState?.phase === 'ready' ? ui.boardState.seedFingerprint ?? null : null);
/** A device account only while its board is connected; a view-only one always. */
const visible = (a) => !ui.forgotten.has(a.uuid) && (a.kind !== 'device' || (!!boardFingerprint() && a.origin?.seedFingerprint === boardFingerprint()));
const visibleAccounts = () => (ui.status?.accounts ?? []).filter(visible);
const hiddenAccounts = () => (ui.status?.accounts ?? []).filter((a) => !visible(a));
const account = () => {
  const accts = visibleAccounts();
  const chosen = ui.prefs.account[net()];
  return accts.find((a) => a.uuid === chosen)?.uuid ?? accts[0]?.uuid;
};

async function guarded(label, f) {
  ui.busy = label;
  ui.error = null;
  render();
  try {
    return await f();
  } catch (e) {
    ui.error = String(e?.message ?? e);
  } finally {
    ui.busy = null;
    render();
  }
}

// ------------------------------------------------------------ actions

async function openNetwork() {
  const s = await ui.engine.call('open', { network: net(), server: server() });
  ui.status = s;
  ui.balances = null;
  ui.addresses = null;
  ui.rotated = null;
  ui.history = [];
  await refresh();
}

/** Drops the data of the account shown before, when another (or none) is shown now. */
function showAccount(acct) {
  if (ui.shown === (acct ?? null)) return;
  ui.shown = acct ?? null;
  ui.balances = null;
  ui.addresses = null;
  ui.rotated = null;
  ui.history = [];
  ui.verified = null;
  ui.proposal = null;
  ui.lastTx = null;
  ui.confirmForget = false;
}

async function refresh() {
  ui.status = await ui.engine.call('status');
  const acct = account();
  showAccount(acct);
  if (!acct) return;
  const addresses = await ui.engine.call('addresses', { account: acct });
  const balances = await ui.engine.call('balances', { account: acct });
  const history = await ui.engine.call('history', { account: acct, limit: 10 });
  // The board may have gone while the engine answered.
  if (account() !== acct) return;
  ui.addresses ??= addresses;
  ui.balances = balances;
  ui.history = history;
  // Shown on the next open while a sync holds the engine (calls queue
  // behind it), and only while this account is. Not for a hidden
  // (passphrase) wallet: its balance and addresses stay out of storage.
  if (!hiddenWallet()) await chrome.storage.local.set({ [`cache-${net()}`]: { status: ui.status, shown: acct, balances, addresses, history } });
}

/** The last answers, while a sync holds the engine: an account's data only while it is shown. */
async function loadCache() {
  const key = `cache-${net()}`;
  const c = (await chrome.storage.local.get(key))[key];
  if (!c) return;
  ui.status = c.status;
  showAccount(account());
  if (c.shown && c.shown === account()) Object.assign(ui, { balances: c.balances, addresses: c.addresses, history: c.history });
}

/** The board came or went: show or hide its account's data. */
function onBoardChanged() {
  const acct = account();
  if ((acct ?? null) === ui.shown || !ui.status) return;
  showAccount(acct);
  render();
  if (!acct) return;
  // From the cache at once (a sync may hold the engine), then fresh.
  void loadCache()
    .then(() => (render(), refresh()))
    .then(render, (e) => {
      ui.error = String(e?.message ?? e);
      render();
    });
}

async function sync() {
  if (ui.syncing) return;
  ui.syncing = true;
  ui.error = null;
  render();
  try {
    const r = await ui.engine.call('sync', {});
    if (r.cancelled && !ui.quietCancel) ui.error = 'Sync stopped. The next sync carries on from here.';
    await refresh();
  } catch (e) {
    ui.error = String(e?.message ?? e);
  } finally {
    ui.syncing = false;
    render();
  }
}

async function importKey(form) {
  const ufvk = form.ufvk.value.trim();
  const birthday = form.birthday.value.trim();
  const params = {
    ufvk,
    name: form.name.value.trim() || 'Account',
    birthday: birthday ? Number(birthday) : null,
  };
  if (form.device.checked) {
    params.origin = {
      seedFingerprint: form.fingerprint.value.trim().toLowerCase(),
      accountIndex: Number(form.index.value || 0),
    };
  }
  await guarded('Importing…', async () => {
    const r = await ui.engine.call('importAccount', params);
    ui.prefs.account[net()] = r.account.uuid;
    await savePrefs();
    ui.addresses = r.addresses;
    await refresh();
  });
  void sync();
}

async function switchNetwork(n) {
  if (n === net()) return;
  // The board answers for one network (its Settings → Network): the
  // session ends, and the USB port with it; connect again on the new one.
  await disconnectBoard();
  if (ui.syncing) await ui.engine.call('cancelSync');
  ui.prefs.network = n;
  ui.proposal = null;
  ui.needBirthday = null;
  ui.verified = null;
  ui.lastTx = null;
  ui.confirmForget = false;
  await savePrefs();
  await guarded('Opening…', openNetwork);
  // Hidden accounts sync too; nothing of theirs is shown.
  if (ui.status?.accounts?.length) void sync();
}

async function saveServer(value) {
  const url = value.trim();
  if (url && !/^https?:\/\//.test(url)) {
    ui.error = 'The endpoint must be an http(s) URL of a gRPC-web proxy.';
    render();
    return;
  }
  if (url && !Object.values(DEFAULT_SERVERS).includes(url)) {
    // Asked from a click: Chrome shows the permission prompt.
    const origin = `${new URL(url).origin}/*`;
    const ok = await chrome.permissions.request({ origins: [origin] });
    if (!ok) {
      ui.error = `No permission to reach ${origin}.`;
      render();
      return;
    }
  }
  ui.prefs.servers[net()] = url;
  await savePrefs();
  await guarded('Saving…', async () => {
    await ui.engine.call('setServer', { server: server() });
    ui.status = await ui.engine.call('status');
  });
}

// ------------------------------------------------------------- board

const boardReady = () => ui.boardState?.phase === 'ready';
/** The board has a passphrase applied: a hidden wallet, another seed fingerprint. */
const hiddenWallet = () => boardReady() && !!ui.boardState.passphrase;

/**
 * Ends the board's session, whichever link: the board's wallet is hidden
 * at once (the host publishes "disconnected" before it closes the link).
 * The offscreen document is told too, so no USB port stays open there.
 */
async function disconnectBoard() {
  const b = ui.board;
  const usb = b?.kind === 'usb' ? b.run('disconnect') : ui.engine.call('board.disconnect');
  const local = b && b.kind !== 'usb' ? b.run('disconnect') : null;
  ui.passHint = null;
  await Promise.all([usb, local].map((p) => p?.catch(() => {})));
}
const acctInfo = () => (ui.status?.accounts ?? []).find((a) => a.uuid === account());

function useBoard(b) {
  ui.board = b;
  ui.boardState = b.state;
  b.onState((s) => {
    if (ui.board !== b) return; // a board this page no longer uses
    ui.boardState = s;
    if (s.task?.op === 'sign') ui.signing = signLine(s.task) ?? ui.signing;
    if (s.phase === 'ready') ui.switching = null;
    render();
    onBoardChanged();
  });
  onBoardChanged();
}

function signLine(t) {
  if (t.phase === 'upload') return `Uploading to the board: ${t.sent} of ${t.total} bytes…`;
  if (t.phase === 'review') return 'Review the transaction on the board, page by page; ACCEPT on the last page signs.';
  if (t.phase === 'finish') return `Signed on the board (${t.signatures} signature${t.signatures === 1 ? '' : 's'}). Proving…`;
  return null;
}

/** Opens this page as a tab, where Chrome's device choosers work. */
async function openTab(connect) {
  await chrome.tabs.create({ url: chrome.runtime.getURL(`popup.html?tab&connect=${connect}`) });
  window.close();
}

/**
 * "Connect over USB". With a board port already granted (the browser's
 * chooser was used once, in a tab) the offscreen document opens it: no
 * tab, from the popup too. With none, the chooser is needed, and only a
 * tab can show it.
 */
async function connectUsb() {
  ui.needUsbGrant = false;
  ui.switching = null;
  ui.error = null;
  const ports = await ui.engine.call('board.usbPorts').catch(() => ({ serial: true, granted: 0 }));
  const plan = usbPlan(ports);
  if (plan === 'noSerial') {
    ui.error = 'This browser has no Web Serial: use desktop Chrome, Edge or Brave.';
    return render();
  }
  if (plan === 'openTab') return openTab('usb');
  if (plan === 'choose') {
    ui.needUsbGrant = true;
    return render();
  }
  await connectRemoteUsb();
}

async function connectRemoteUsb() {
  if (ui.board?.kind !== 'usb') useBoard(new RemoteBoard(ui.engine));
  ui.usbTabOnly = false;
  await guarded('Connecting to the board…', async () => {
    try {
      await ui.board.run('connect', { kind: 'usb', network: net() });
    } catch (e) {
      // The grant went (the board was unplugged and is another port now, say).
      if (e.need === 'usbPermission') {
        ui.needUsbGrant = true;
        if (!inTab()) await openTab('usb');
        return;
      }
      throw e;
    }
  });
}

async function grantAndConnectUsb() {
  // Straight from the click: requestPort needs the user's gesture.
  let port;
  try {
    port = await grantUsb();
  } catch (e) {
    ui.error = String(e?.message ?? e);
    return render();
  }
  if (!port) {
    ui.error = 'No board was chosen. If the list was empty: is the cable a data cable, and is the board set to USB (Settings → Link)?';
    return render();
  }
  ui.needUsbGrant = false;
  // The grant is the extension's: the offscreen document sees it, and the
  // board is held there (it outlives this tab, and the popup finds it).
  const ports = await ui.engine.call('board.usbPorts').catch(() => ({ serial: true, granted: 0 }));
  if (ports.granted > 0) return connectRemoteUsb();
  // A browser that keeps the grant for this tab only: hold the port here,
  // as Bluetooth is held, and say so.
  await disconnectBoard();
  const local = new LocalBoard(ui.engine, { serialPort: port });
  ui.usbTabOnly = true;
  useBoard(local);
  await guarded('Connecting to the board…', () => local.run('connect', { kind: 'usb', network: net() }));
}

async function connectBluetooth() {
  // The popup only opens the tab: the chooser needs one, and whether the
  // browser has Web Bluetooth is checked there (an extension popup may
  // lack navigator.bluetooth where a tab has it).
  if (!inTab()) return openTab('ble');
  if (!navigator.bluetooth) {
    ui.error = bluetoothHelp();
    return render();
  }
  const local = new LocalBoard(ui.engine);
  try {
    await local.chooseBluetooth(); // the click's gesture
  } catch (e) {
    // Closing the chooser is a NotFoundError too; its text says which.
    ui.error = `Bluetooth: ${e?.message ?? e}`;
    render();
    return;
  }
  // The USB session, if any, ends and its port is released: the board
  // serves one link at a time. So does an earlier Bluetooth one here.
  await disconnectBoard();
  ui.error = null;
  ui.switching = null;
  useBoard(local);
  await guarded('Connecting over Bluetooth…', () => local.run('connect', { kind: 'ble', network: net() }));
}

/** From a USB connection: end it, and say what to do next. */
async function switchToBluetooth() {
  await guarded(null, disconnectBoard);
  ui.switching = 'ble';
  render();
}

/**
 * The weaker path to a hidden wallet: the passphrase typed here. It goes
 * from the field to the board and the field is emptied whatever happens;
 * it is never stored, logged, or put in the page's state.
 */
async function sendPassphrase(input) {
  const complaint = passphraseComplaint(input.value);
  if (complaint) {
    ui.passHint = complaint;
    input.value = '';
    return render();
  }
  ui.passHint = null;
  const pending = ui.board.run('setPassphrase', { passphrase: input.value, network: net() });
  input.value = '';
  await guarded('Check the fingerprint (XFP) on the board, then confirm there…', async () => {
    try {
      const r = await pending;
      ui.passHint = r.xfp
        ? `Applied. The board showed XFP ${r.xfp}: if that is not the wallet you expected, the passphrase was mistyped. Disconnect or lock the board to drop it.`
        : 'Applied. Check the XFP against the one the board showed.';
    } catch (e) {
      ui.passHint = String(e?.message ?? e);
    }
  });
}

/**
 * One field for the whole page's life: render() redraws everything (a sync
 * does, every batch), and a new field would drop what is being typed.
 */
let passInput = null;

function passphraseSection() {
  const s = ui.boardState;
  passInput ??= h('input', { type: 'password', id: 'passphrase', autocomplete: 'off', spellcheck: 'false', autocapitalize: 'off', 'aria-label': 'Passphrase', maxlength: 63 });
  const input = passInput;
  const busy = !!ui.busy || !!ui.signing;
  return h(
    'details',
    { id: 'passpanel', open: ui.passOpen ?? (!!s.passphrase || !!ui.passHint), ontoggle: (e) => (ui.passOpen = e.target.open) },
    h('summary', {}, 'Passphrase (hidden wallet)'),
    h(
      'p',
      { id: 'pass-state', class: s.passphrase ? 'notice' : 'muted' },
      s.passphrase ? `Hidden wallet open: a passphrase is applied on the board${s.xfp ? ` (XFP ${s.xfp})` : ''}.` : 'No passphrase applied: the wallet without one.',
    ),
    h(
      'p',
      { class: 'muted' },
      'A passphrase opens a separate wallet from the same seed: another seed fingerprint, other addresses, another balance. This wallet keeps it apart by its fingerprint and never mixes it with the one without. The board forgets the passphrase on lock and on disconnect, and so does this wallet.',
    ),
    h(
      'p',
      { class: 'muted' },
      'Its synced data (what its viewing key has seen, never a key that spends) is kept in this browser under that fingerprint, so anyone with this browser profile can tell a second wallet was opened here. "Forget this board", with the hidden wallet connected, deletes it.',
    ),
    h('p', {}, h('strong', {}, 'Enter it on the board'), ' (safer): Settings → Passphrase on the board. Its keyboard is slower, and it is the only way the passphrase never leaves the board. This wallet follows within a few seconds.'),
    h(
      'details',
      { id: 'passdisclose', open: !!ui.passDisclose, ontoggle: (e) => (ui.passDisclose = e.target.open) },
      h('summary', {}, 'Type it here instead (less safe)'),
      h(
        'div',
        { class: 'notice bad' },
        h('p', {}, 'Typing a passphrase here is weaker than typing it on the board, and the encrypted link does not change that: this browser sees every character before anything is encrypted, so anything that has this computer (malware, a screen recorder, an extension that reads pages) sees it too.'),
        h('p', {}, 'A wrong passphrase does not fail: it opens a different, empty wallet. The board shows the fingerprint (XFP) of the seed it derived; check it before you use that wallet, and confirm on the board.'),
      ),
      h('label', {}, h('span', {}, 'Passphrase'), input),
      h('div', { class: 'row' }, h('button', { id: 'passapply', onclick: () => void sendPassphrase(input), disabled: busy }, 'Send to the board')),
    ),
    ui.passHint ? h('p', { class: 'notice', id: 'passhint' }, ui.passHint) : null,
  );
}

/** "Forget this board": its accounts on this network, and all synced for them. */
async function forgetBoard() {
  const fp = boardFingerprint();
  const mine = (ui.status?.accounts ?? []).filter((a) => a.kind === 'device' && fp && a.origin?.seedFingerprint === fp);
  ui.confirmForget = false;
  // Hidden now: the deletion waits for the engine, which a sync holds
  // until it reaches the end of its batch.
  for (const a of mine) ui.forgotten.add(a.uuid);
  showAccount(account());
  await guarded('Forgetting this board…', async () => {
    await chrome.storage.local.remove(`cache-${net()}`);
    delete ui.prefs.account[net()];
    await savePrefs();
    // A sync holds the engine until it ends: stop it first.
    if (ui.syncing) {
      ui.quietCancel = true;
      await ui.engine.call('cancelSync');
    }
    try {
      for (const a of mine) await ui.engine.call('deleteAccount', { account: a.uuid });
      showAccount(null);
      await refresh();
    } finally {
      ui.quietCancel = false;
      // Gone from the engine's status by now (or, on an error, shown again).
      for (const a of mine) ui.forgotten.delete(a.uuid);
    }
  });
}

async function importFromBoard(birthday = null) {
  await guarded('Reading the account from the board…', async () => {
    try {
      const hidden = hiddenWallet();
      const r = await ui.board.run('importAccount', { network: net(), accountIndex: 0, birthday, name: hidden ? 'LeekWallet #0 (passphrase)' : null });
      ui.needBirthday = null;
      if (!hidden) {
        ui.prefs.account[net()] = r.account.uuid;
        await savePrefs();
      }
      ui.addresses = r.addresses;
      await refresh();
    } catch (e) {
      if (e.need === 'birthday') {
        ui.needBirthday = { tip: e.tip };
        return;
      }
      throw e;
    }
  });
  if (account() && !ui.needBirthday) void sync();
}

async function verifyOnBoard(ua) {
  const a = acctInfo();
  ui.verified = null;
  await guarded('Compare the address on the board and answer there…', async () => {
    await ui.board.run('showAddress', { network: net(), accountIndex: a.origin.accountIndex, ua });
    ui.verified = ua;
  });
}

/** "1.5" -> 150000000 zatoshi, exactly (no floating point). */
export function parseZec(text) {
  const m = /^(\d+)(?:\.(\d{0,8}))?$/.exec(String(text).trim());
  if (!m) throw new Error('Enter the amount in ZEC, with at most 8 decimals.');
  const zat = BigInt(m[1]) * 100000000n + BigInt((m[2] ?? '').padEnd(8, '0'));
  if (zat <= 0n || zat > 2100000000000000n) throw new Error('That amount is not possible.');
  return Number(zat);
}

async function propose(kind, form) {
  ui.proposal = null;
  ui.lastTx = null;
  ui.confirmMainnet = false;
  await guarded('Building the transaction…', async () => {
    let p;
    if (kind === 'shield') {
      p = await ui.engine.call('proposeShield', { account: account(), compact: true });
    } else {
      const to = form.to.value.trim();
      const amountZat = parseZec(form.amount.value);
      const memo = form.memo.value;
      p = await ui.engine.call('proposeSend', { account: account(), to, amountZat, memo: memo || null, compact: true });
    }
    ui.proposal = { kind, ...p };
  });
}

async function signProposal() {
  const p = ui.proposal;
  const a = acctInfo();
  if (net() === 'main' && !ui.confirmMainnet) {
    ui.error = 'This moves real ZEC on mainnet. Tick the confirmation above first.';
    return render();
  }
  ui.signing = 'Starting…';
  ui.lastTx = null;
  await guarded(null, async () => {
    try {
      ui.lastTx = await ui.board.run('signAndFinish', {
        network: net(),
        accountIndex: a.origin.accountIndex,
        signerView: p.signerView,
        broadcast: true,
        confirmMainnet: net() === 'main' && ui.confirmMainnet,
      });
      ui.proposal = null;
    } finally {
      ui.signing = null;
    }
  });
  await guarded(null, refresh);
}

function boardSection() {
  const s = ui.boardState ?? { phase: 'disconnected' };
  const busy = !!ui.busy || !!ui.signing;
  const lines = [];
  if (s.phase === 'disconnected' || s.phase === 'connecting') {
    if (hiddenAccounts().length) lines.push(h('p', { class: 'notice', id: 'connect-to-see' }, 'Connect your LeekWallet board to see this wallet.'));
    lines.push(
      ui.switching === 'ble'
        ? h('p', { class: 'notice', id: 'switch-steps' }, 'USB disconnected. On the board, set Settings → Link to Bluetooth, then press Bluetooth here.')
        : h('p', { class: 'muted' }, 'Sign on your LeekWallet board. It serves one link at a time, USB or Bluetooth, set on the board (Settings → Link). To switch: Disconnect, change Link on the board, then use the other button.'),
      ui.needUsbGrant && inTab()
        ? h('div', { class: 'row' }, h('button', { class: 'primary', id: 'grant-usb', onclick: () => void grantAndConnectUsb() }, 'Choose the board (USB)'))
        : h(
            'div',
            { class: 'row' },
            h('button', { id: 'connect-usb', onclick: () => void connectUsb(), disabled: busy || s.phase === 'connecting' }, 'Connect over USB'),
            h('button', { id: 'connect-ble', class: ui.switching === 'ble' ? 'primary' : null, onclick: () => void connectBluetooth(), disabled: busy || s.phase === 'connecting' }, 'Bluetooth'),
          ),
      inTab() && query.get('connect') === 'usb' && ui.needUsbGrant ? h('p', { class: 'notice', id: 'usb-why' }, USB_TAB_WHY) : null,
      inTab()
        ? navigator.bluetooth
          ? query.get('connect') === 'ble' && !ui.board?.kind?.startsWith('ble')
            ? h('p', { class: 'muted', id: 'ble-press' }, 'Press Bluetooth to choose the board in the browser\'s list.')
            : null
          : h('p', { class: 'notice', id: 'ble-help' }, bluetoothHelp())
        : h('p', { class: 'muted', id: 'ble-why' }, BLE_TAB_WHY),
    );
  } else if (s.phase === 'pairing') {
    lines.push(
      h('p', {}, 'Check that the board shows the same six digits, then press ALLOW on the board:'),
      h('p', { class: 'big mono', id: 'passkey' }, s.passkey ?? '…'),
      h('p', { class: 'muted' }, 'If they differ, deny it on the board: something is between this browser and the board.'),
      h('button', { onclick: () => ui.board.run('cancel') }, 'Cancel'),
    );
  } else if (s.phase === 'unlocking') {
    const line = s.waiting === 'pin' ? 'Enter your PIN on the board.' : s.waiting === 'wallet' ? 'Unlocked. Reading which wallet the board holds…' : 'Paired. Checking that the board is unlocked…';
    lines.push(h('p', { id: 'board-pin' }, line), h('button', { onclick: () => ui.board.run('cancel') }, 'Cancel'));
  } else if (s.phase === 'ready') {
    const fp = boardFingerprint();
    const known = fp && (ui.status?.accounts ?? []).some((a) => a.kind === 'device' && !ui.forgotten.has(a.uuid) && a.origin?.seedFingerprint === fp);
    lines.push(
      h(
        'div',
        { class: 'row' },
        h('span', { class: 'grow', id: 'board-ready' }, `Connected: ${s.label ?? s.kind}${s.kind === 'ble' || ui.board?.kind === 'usb-tab' ? ' (keep this tab open)' : ''}${s.passphrase ? ' · hidden wallet (passphrase)' : ''}`),
        s.kind === 'usb' ? h('button', { class: 'link', id: 'switch-ble', onclick: () => void switchToBluetooth(), disabled: busy }, 'Switch to Bluetooth') : null,
        h('button', { class: 'link', id: 'disconnect', onclick: () => void guarded(null, disconnectBoard), disabled: busy }, 'Disconnect'),
      ),
      ui.usbTabOnly && ui.board?.kind === 'usb-tab'
        ? h('p', { class: 'notice', id: 'usb-tab-only' }, 'This browser gave the USB port to this tab only, so the board is connected from here: keep this tab open while you use it.')
        : null,
      s.locked
        ? h('p', { class: 'notice', id: 'board-locked' }, 'The board locked, and dropped any passphrase. Unlock it with your PIN on the board to see its wallet.')
        : s.waiting === 'wallet'
          ? h('p', { class: 'muted', id: 'board-reading' }, 'The board\'s wallet changed. Reading which wallet it holds now…')
          : fp
            ? null
            : h('p', { class: 'notice bad', id: 'board-account-error' }, s.accountError ?? 'The board did not say which wallet it holds, so no wallet of a board is shown. Disconnect and connect again.'),
      s.locked ? null : passphraseSection(),
    );
    if (known && ui.confirmForget) {
      lines.push(
        h('p', { class: 'notice', id: 'forget-confirm' }, `Forget this board? Its ${NETWORKS[net()]} account and everything synced for it are deleted from this browser. The board and its funds are not touched; importing it again syncs from its birthday.`),
        h(
          'div',
          { class: 'row' },
          h('button', { class: 'primary', id: 'forget-yes', onclick: () => void forgetBoard(), disabled: busy }, 'Forget'),
          h('button', { onclick: () => ((ui.confirmForget = false), render()) }, 'Keep'),
        ),
      );
    } else if (known) {
      lines.push(h('div', { class: 'row' }, h('button', { class: 'link', id: 'forget', onclick: () => ((ui.confirmForget = true), render()), disabled: busy }, 'Forget this board')));
    }
  }
  if (s.error && s.phase === 'disconnected') lines.push(h('p', { class: 'notice bad' }, s.error));
  return h('section', { id: 'board' }, h('h2', {}, 'Board'), lines);
}

function boardImportSection() {
  if (!boardReady() || !boardFingerprint()) return null;
  if (ui.needBirthday) {
    const form = h(
      'form',
      {
        id: 'birthday-form',
        onsubmit: (e) => {
          e.preventDefault();
          void importFromBoard(Number(form.birthday.value));
        },
      },
      h('p', {}, `The board has no ${NETWORKS[net()]} birthday for this account yet. Enter the block height of its first transaction, or a little before (for a new account, the current height). It is recorded on the board for next time.`),
      h('label', {}, h('span', {}, 'Birthday height'), h('input', { name: 'birthday', type: 'number', min: 1, required: true, value: ui.needBirthday.tip ?? '' })),
      h('button', { class: 'primary', type: 'submit', disabled: !!ui.busy }, 'Import'),
    );
    return h('section', {}, h('h2', {}, 'Import from the board'), form);
  }
  return h(
    'section',
    {},
    h('h2', {}, hiddenWallet() ? 'Import the hidden wallet' : 'Import from the board'),
    h('p', { class: 'muted' }, `The board gives its ${NETWORKS[net()]} viewing key for account 0; the spending keys stay on it.`),
    hiddenWallet() ? h('p', { class: 'muted', id: 'hidden-import' }, 'This is the passphrase wallet: it becomes a separate account here, shown only while the board has this passphrase applied.') : null,
    h('button', { class: 'primary', id: 'import-board', onclick: () => void importFromBoard(), disabled: !!ui.busy }, hiddenWallet() ? "Import the hidden wallet's account" : "Import the board's account"),
  );
}

function reviewBlock() {
  const p = ui.proposal;
  const sent = p.payments.reduce((n, x) => n + x.amountZat, 0);
  const shortAddr = (a) => (a.length > 30 ? `${a.slice(0, 14)}…${a.slice(-10)}` : a);
  return h(
    'div',
    { id: 'review' },
    h('p', {}, p.kind === 'shield' ? 'Shield the transparent coins into this wallet:' : 'You are about to send:'),
    h(
      'table',
      {},
      p.payments.map((x) => h('tr', {}, h('td', {}, h('code', {}, shortAddr(x.recipient)), x.hasMemo ? ' (memo)' : ''), h('td', { class: 'num' }, formatZec(x.amountZat)))),
      h('tr', {}, h('td', {}, 'Fee'), h('td', { class: 'num', id: 'review-fee' }, formatZec(p.feeZat))),
      (p.change ?? []).map((c) => h('tr', { class: 'muted' }, h('td', {}, `Change back to you (${c.pool})`), h('td', { class: 'num' }, formatZec(c.valueZat)))),
      p.kind === 'shield' ? null : h('tr', {}, h('th', {}, 'Total out'), h('th', { class: 'num' }, formatZec(sent + p.feeZat))),
    ),
    h('p', { class: 'muted' }, 'The board shows the same on its own screen. Compare there before you ACCEPT: what the board shows is what it signs.'),
    net() === 'main'
      ? h(
          'label',
          { class: 'check notice' },
          h('input', { type: 'checkbox', id: 'confirm-mainnet', checked: ui.confirmMainnet, onchange: (e) => (ui.confirmMainnet = e.target.checked) }),
          h('span', {}, 'This sends ', h('strong', {}, 'real ZEC on mainnet'), '. I checked the address and amount here and will check them on the board.'),
        )
      : null,
    h(
      'div',
      { class: 'row' },
      h('button', { class: 'primary', id: 'sign', onclick: () => void signProposal(), disabled: !boardReady() || !!ui.busy || !!ui.signing }, boardReady() ? 'Sign on the board' : 'Connect the board to sign'),
      h('button', { onclick: () => ((ui.proposal = null), render()), disabled: !!ui.signing }, 'Cancel'),
    ),
  );
}

function sendSection() {
  const a = acctInfo();
  if (!a || a.kind !== 'device') return null;
  const b = ui.balances?.balances;
  const form = h(
    'form',
    {
      id: 'send',
      onsubmit: (e) => {
        e.preventDefault();
        void propose('send', form);
      },
    },
    h('label', {}, h('span', {}, 'To (unified, Orchard-only or transparent address)'), h('input', { name: 'to', type: 'text', required: true, spellcheck: 'false' })),
    h('label', {}, h('span', {}, 'Amount (ZEC)'), h('input', { name: 'amount', type: 'text', inputmode: 'decimal', required: true, placeholder: '0.001' })),
    h('label', {}, h('span', {}, 'Memo (shielded recipients only)'), h('textarea', { name: 'memo', rows: 2, maxlength: 512 })),
    h('button', { type: 'submit', disabled: !!ui.busy || !!ui.signing }, 'Review'),
  );
  return h(
    'section',
    { id: 'send-section' },
    h('h2', {}, 'Send'),
    ui.proposal ? reviewBlock() : form,
    !ui.proposal && b?.transparent?.spendable > 0
      ? h(
          'div',
          { class: 'row' },
          h('span', { class: 'grow muted' }, `${formatZec(b.transparent.spendable)} transparent can be shielded.`),
          h('button', { id: 'shield', onclick: () => void propose('shield'), disabled: !!ui.busy || !!ui.signing }, 'Shield'),
        )
      : null,
    ui.signing ? h('p', { class: 'notice', id: 'signing' }, ui.signing) : null,
    ui.lastTx
      ? h('p', { class: 'notice', id: 'txid' }, `Sent${ui.lastTx.broadcast ? '' : ' (not broadcast)'}: `, h('code', {}, ui.lastTx.txid))
      : ui.boardState?.lastResult?.network === net() && ui.boardState.lastResult.seedFingerprint === boardFingerprint() && !ui.signing
        ? h('p', { class: 'muted' }, 'Last signed on the board: ', h('code', {}, ui.boardState.lastResult.txid))
        : null,
  );
}

// ------------------------------------------------------------- render

function syncSection() {
  const s = ui.status?.sync;
  const ev = ui.sync;
  let pct = null;
  let line = 'Not synced yet.';
  if (s) {
    pct = s.progressDen > 0 ? (100 * s.progressNum) / s.progressDen : null;
    line = `Scanned to ${s.fullyScanned.toLocaleString()} of ${s.chainTip.toLocaleString()}`;
  }
  if (ui.syncing && ev?.phase === 'scanned') {
    const left = Math.max(0, ev.tip - (ui.syncStart ?? ev.start));
    line = `Syncing: block ${ev.end.toLocaleString()} of ${ev.tip.toLocaleString()} (${ev.scannedBlocks.toLocaleString()} scanned)`;
    if (left > 0) pct = Math.min(100, (100 * ev.scannedBlocks) / left);
  } else if (ui.syncing) {
    line = 'Syncing: connecting…';
  }
  return h(
    'section',
    { id: 'sync' },
    h('h2', {}, 'Sync'),
    h('progress', { max: 100, value: pct ?? 0, 'aria-label': 'sync progress' }),
    h('p', { class: 'muted', id: 'sync-line' }, line),
    h(
      'div',
      { class: 'row' },
      ui.syncing
        ? h('button', { onclick: () => ui.engine.call('cancelSync') }, 'Stop')
        : h('button', { onclick: () => void sync(), disabled: !account() }, 'Sync now'),
      h('span', { class: 'muted grow' }, `via ${new URL(server()).host}`),
    ),
  );
}

function balanceSection() {
  const b = ui.balances?.balances;
  const pools = ['orchard', 'ironwood', 'transparent'];
  return h(
    'section',
    { id: 'balances' },
    h('h2', {}, 'Balance'),
    b
      ? [
          h('p', { class: 'big', id: 'total' }, formatZec(b.total)),
          h('p', { class: 'muted' }, `${formatZec(b.spendable)} spendable`),
          h(
            'table',
            {},
            h('tr', {}, h('th', {}, 'Pool'), h('th', { class: 'num' }, 'Total'), h('th', { class: 'num' }, 'Spendable')),
            pools.map((p) =>
              h(
                'tr',
                {},
                h('td', {}, p[0].toUpperCase() + p.slice(1)),
                h('td', { class: 'num' }, formatZec(b[p].total)),
                h('td', { class: 'num' }, formatZec(b[p].spendable)),
              ),
            ),
          ),
        ]
      : h('p', { class: 'muted' }, 'No balance yet: sync first.'),
  );
}

function receiveSection() {
  const a = ui.rotated ?? ui.addresses;
  if (!a) return null;
  const copy = (text) => navigator.clipboard.writeText(text);
  return h(
    'section',
    { id: 'receive' },
    h('h2', {}, 'Receive'),
    h('p', {}, h('code', { id: 'ua' }, a.unified)),
    h('p', { class: 'muted' }, `Unified address, diversifier index ${a.diversifierIndex}.`),
    a.transparent ? h('p', { class: 'muted' }, 'Transparent: ', h('code', {}, a.transparent)) : null,
    h(
      'div',
      { class: 'row' },
      h('button', { onclick: () => copy(a.unified) }, 'Copy'),
      h(
        'button',
        {
          onclick: () =>
            guarded('New address…', async () => {
              ui.rotated = await ui.engine.call('nextAddress', { account: account() });
            }),
        },
        'New address',
      ),
    ),
    acctInfo()?.kind === 'device'
      ? h(
          'div',
          { class: 'row' },
          h('button', { id: 'verify', onclick: () => void verifyOnBoard(a.unified), disabled: !boardReady() || !!ui.busy }, 'Verify on the board'),
          ui.verified === a.unified ? h('span', { class: 'grow', id: 'verified' }, 'The board confirmed this address.') : null,
        )
      : null,
    ui.verified === a.unified
      ? null
      : h('p', { class: 'notice' }, acctInfo()?.kind === 'device' ? 'Before receiving anything that matters, check this address on the board: connect it and press Verify.' : 'A view-only account: nothing here can check this address.'),
  );
}

function historySection() {
  if (!ui.history?.length) return null;
  return h(
    'section',
    { id: 'history' },
    h('h2', {}, 'Recent'),
    h(
      'ul',
      { class: 'history' },
      ui.history.map((t) => {
        const { label, amount } = historyEntry(t);
        return h('li', {}, h('span', {}, label), h('span', { class: 'mono' }, amount));
      }),
    ),
  );
}

function importSection() {
  const form = h(
    'form',
    {
      id: 'import',
      onsubmit: (e) => {
        e.preventDefault();
        void importKey(form);
      },
    },
    h('h2', {}, 'Import a viewing key'),
    h('p', { class: 'muted' }, `Watch a ${NETWORKS[net()]} account: the wallet syncs and shows balances with no spending key here.`),
    h('label', {}, h('span', {}, 'Unified full viewing key (Orchard + transparent)'), h('textarea', { name: 'ufvk', rows: 4, required: true, spellcheck: 'false' })),
    h('label', {}, h('span', {}, 'Birthday height (empty: a new account, from the chain tip)'), h('input', { name: 'birthday', type: 'number', min: 1 })),
    h('label', {}, h('span', {}, 'Name'), h('input', { name: 'name', type: 'text', value: 'Account' })),
    h('label', { class: 'check' }, h('input', { name: 'device', type: 'checkbox' }), 'Device account (its key is on a LeekWallet board)'),
    h('label', {}, h('span', {}, 'ZIP 32 seed fingerprint (hex), device accounts'), h('input', { name: 'fingerprint', type: 'text', spellcheck: 'false' })),
    h('label', {}, h('span', {}, 'Account index'), h('input', { name: 'index', type: 'number', min: 0, value: 0 })),
    h('button', { class: 'primary', type: 'submit', disabled: !!ui.busy }, 'Import'),
  );
  return h('section', {}, form);
}

function settingsSection() {
  const input = h('input', { type: 'text', value: ui.prefs.servers[net()] ?? '', placeholder: DEFAULT_SERVERS[net()], spellcheck: 'false' });
  const accts = visibleAccounts();
  return h(
    'details',
    {},
    h('summary', {}, 'Settings'),
    h(
      'section',
      {},
      h('label', {}, h('span', {}, `gRPC-web endpoint for ${NETWORKS[net()]} (empty: ChainSafe's proxy)`), input),
      h('div', { class: 'row' }, h('button', { onclick: () => void saveServer(input.value) }, 'Save endpoint')),
      h(
        'p',
        { class: 'muted' },
        'The proxy sees your IP address and the transactions you broadcast, never your keys. Everything it returns is checked against the commitment trees.',
      ),
      accts.length > 1
        ? h(
            'label',
            {},
            h('span', {}, 'Account'),
            h(
              'select',
              {
                onchange: async (e) => {
                  ui.prefs.account[net()] = e.target.value;
                  await savePrefs();
                  ui.addresses = null;
                  ui.rotated = null;
                  await guarded('Loading…', refresh);
                },
              },
              accts.map((a) => h('option', { value: a.uuid, selected: a.uuid === account() }, `${a.name ?? 'Account'} (${a.kind})`)),
            ),
          )
        : null,
      accts.length ? h('p', { class: 'muted' }, `Wallet database: ${(ui.status.dbBytes / 1e6).toFixed(1)} MB on this browser's storage.`) : null,
      h(
        'button',
        {
          class: 'link',
          onclick: async () => {
            if (!confirm(`Delete the ${NETWORKS[net()]} wallet from this browser? Viewing keys can be imported again; the sync starts over.`)) return;
            await guarded('Deleting…', async () => {
              await ui.engine.call('wipe', { network: net() });
              await openNetwork();
            });
          },
        },
        `Delete the ${NETWORKS[net()]} wallet`,
      ),
    ),
  );
}

/** Sections may be null (not shown) or arrays of nodes. */
function replaceRoot(...parts) {
  root.replaceChildren(...parts.flat(Infinity).filter((n) => n instanceof Node));
}

function render() {
  const typing = passInput && document.activeElement === passInput;
  renderPage();
  if (typing && passInput.isConnected) passInput.focus();
}

function renderPage() {
  const host = ui.engine?.host;
  const engineOk = host?.ready;
  const acct = (ui.status?.accounts ?? []).find((a) => a.uuid === account());
  const select = h(
    'select',
    { id: 'network', 'aria-label': 'network', onchange: (e) => void switchNetwork(e.target.value), disabled: !!ui.busy },
    Object.entries(NETWORKS).map(([k, v]) => h('option', { value: k, selected: k === net() }, v)),
  );
  replaceRoot(
    h(
      'header',
      {},
      h('span', { class: `dot ${!engineOk ? 'off' : ui.syncing ? 'busy' : 'on'}`, 'aria-hidden': 'true' }),
      h('h1', {}, 'LeekWallet Zcash'),
      select,
    ),
    !engineOk
      ? h('p', { class: host?.fatal ? 'notice bad' : 'muted' }, host?.fatal ? `Engine stopped: ${host.fatal}` : 'Starting the wallet engine…')
      : null,
    net() === 'main' ? h('p', { class: 'notice bad', id: 'mainnet-bar' }, 'MAINNET: real funds. This build is a preview.') : null,
    ui.error ? h('p', { class: 'notice bad', id: 'error' }, ui.error) : null,
    ui.busy ? h('p', { class: 'muted' }, ui.busy) : null,
    acct
      ? [
          h(
            'p',
            { class: 'muted', id: 'account' },
            `${acct.name ?? 'Account'} · ${acct.kind === 'device' ? `device ${acct.shortFingerprint} #${acct.origin.accountIndex}` : 'view only'} · birthday ${acct.birthday.toLocaleString()}`,
          ),
          balanceSection(),
          boardSection(),
          sendSection(),
          syncSection(),
          receiveSection(),
          historySection(),
        ]
      : engineOk && ui.status
        ? hiddenAccounts().length
          ? // A board's wallet is stored here: nothing but the board until it connects.
            [boardSection(), boardImportSection()]
          : [boardSection(), boardImportSection(), importSection()]
        : null,
    engineOk && ui.status ? settingsSection() : null,
  );
}

// --------------------------------------------------------------- start

async function main() {
  await loadPrefs();
  ui.engine = await EngineClient.connect();
  useBoard(new RemoteBoard(ui.engine));
  ui.engine.onHost((hs) => {
    const was = ui.syncing;
    ui.syncing = hs.busy === 'sync';
    if (hs.lastSync) ui.sync = hs.lastSync;
    // A sync this popup did not start (it was open before) just ended.
    if (was && !ui.syncing && !ui.busy) void guarded('', refresh);
    render();
  });
  ui.engine.onEvent((ev) => {
    if (ev.event === 'sync') {
      ui.sync = ev;
      if (ev.phase === 'started') ui.syncStart = ev.fullyScanned ?? null;
      const line = document.getElementById('sync-line');
      if (line) render();
    }
  });
  render();
  const hs = await ui.engine.ready();
  ui.syncing = hs.busy === 'sync';
  ui.sync = hs.lastSync;
  await guarded('Opening…', async () => {
    // The engine may already have this network open (and be syncing it).
    if (hs.network === net() && ui.syncing) {
      await loadCache();
      // No cache (an account imported from another page, say): the
      // offscreen document's last status still lists the accounts.
      if (!ui.status && hs.status?.network === net()) {
        ui.status = hs.status;
        showAccount(account());
      }
    } else if (hs.network === net()) {
      await refresh();
    } else {
      await openNetwork();
    }
  });
  // Opened for the USB chooser: unless a port is granted by now, say why
  // and offer it; with one, connect here at once.
  if (query.get('connect') === 'usb') {
    const ports = await ui.engine.call('board.usbPorts').catch(() => ({ serial: true, granted: 0 }));
    if (usbPlan(ports, true) === 'connect') void connectRemoteUsb();
    else ui.needUsbGrant = true;
  }
  render();
  // Hidden accounts sync too; nothing of theirs is shown.
  if (ui.status?.accounts?.length && !ui.syncing) void sync();
}

// `?bare`: the page without the popup's own logic, for test/e2e.mjs,
// which drives the engine itself from this extension page.
if (!query.has('bare')) main().catch((e) => {
  root.replaceChildren(h('p', { class: 'notice bad' }, `The wallet engine did not start: ${e?.message ?? e}`));
});
