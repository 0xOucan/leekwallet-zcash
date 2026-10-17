// The popup: network switch, import a viewing key, sync progress,
// balances per pool, receive address (with rotation), recent history,
// and the board (9.3): connect over USB or Bluetooth, import its account,
// check an address on its screen, send and shield signed on it.
//
// It owns nothing: every answer comes from the engine through
// EngineClient, and the few choices it remembers (network, endpoint,
// account) are in chrome.storage.local. Text from the chain (memos,
// addresses) only ever goes into textContent.

import { EngineClient } from './engine-client.js';
import { LocalBoard, RemoteBoard, grantUsb, inTab } from './board-client.js';

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

export function formatZec(zat) {
  const z = BigInt(zat ?? 0);
  const neg = z < 0n;
  const a = neg ? -z : z;
  const s = `${a / 100000000n}.${(a % 100000000n).toString().padStart(8, '0')}`;
  return `${neg ? '-' : ''}${s} ZEC`;
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
const account = () => {
  const accts = ui.status?.accounts ?? [];
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

async function refresh() {
  ui.status = await ui.engine.call('status');
  const acct = account();
  if (!acct) return;
  ui.addresses ??= await ui.engine.call('addresses', { account: acct });
  ui.balances = await ui.engine.call('balances', { account: acct });
  ui.history = await ui.engine.call('history', { account: acct, limit: 10 });
  // Shown on the next open while a sync holds the engine (calls queue
  // behind it).
  const { status, balances, addresses, history } = ui;
  await chrome.storage.local.set({ [`cache-${net()}`]: { status, balances, addresses, history } });
}

async function loadCache() {
  const key = `cache-${net()}`;
  const c = (await chrome.storage.local.get(key))[key];
  if (c) Object.assign(ui, c);
}

async function sync() {
  if (ui.syncing) return;
  ui.syncing = true;
  ui.error = null;
  render();
  try {
    const r = await ui.engine.call('sync', {});
    if (r.cancelled) ui.error = 'Sync stopped. The next sync carries on from here.';
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
  if (ui.syncing) await ui.engine.call('cancelSync');
  ui.prefs.network = n;
  ui.proposal = null;
  ui.needBirthday = null;
  ui.verified = null;
  ui.lastTx = null;
  await savePrefs();
  await guarded('Opening…', openNetwork);
  if (account()) void sync();
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
const acctInfo = () => (ui.status?.accounts ?? []).find((a) => a.uuid === account());

function useBoard(b) {
  ui.board = b;
  ui.boardState = b.state;
  b.onState((s) => {
    ui.boardState = s;
    if (s.task?.op === 'sign') ui.signing = signLine(s.task) ?? ui.signing;
    render();
  });
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

async function connectUsb() {
  if (ui.board?.kind !== 'usb') useBoard(new RemoteBoard(ui.engine));
  ui.needUsbGrant = false;
  await guarded('Connecting to the board…', async () => {
    try {
      await ui.board.run('connect', { kind: 'usb' });
    } catch (e) {
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
  let chosen;
  try {
    chosen = await grantUsb();
  } catch (e) {
    ui.error = String(e?.message ?? e);
    return render();
  }
  if (!chosen) {
    ui.error = 'No board was chosen. If the list was empty: is the cable a data cable, and is the board set to USB (Settings → Link)?';
    return render();
  }
  await connectUsb();
}

async function connectBluetooth() {
  if (!inTab()) return openTab('ble');
  const local = new LocalBoard(ui.engine);
  try {
    await local.chooseBluetooth(); // the click's gesture
  } catch (e) {
    if (e?.name !== 'NotFoundError') {
      ui.error = String(e?.message ?? e);
      render();
    }
    return;
  }
  useBoard(local);
  await guarded('Connecting over Bluetooth…', () => local.run('connect', { kind: 'ble' }));
}

async function importFromBoard(birthday = null) {
  await guarded('Reading the account from the board…', async () => {
    try {
      const r = await ui.board.run('importAccount', { network: net(), accountIndex: 0, birthday });
      ui.needBirthday = null;
      ui.prefs.account[net()] = r.account.uuid;
      await savePrefs();
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
    lines.push(
      h('p', { class: 'muted' }, 'Sign on your LeekWallet board. It serves one link at a time, USB or Bluetooth, set on the board (Settings → Link).'),
      ui.needUsbGrant && inTab()
        ? h('div', { class: 'row' }, h('button', { class: 'primary', id: 'grant-usb', onclick: () => void grantAndConnectUsb() }, 'Choose the board (USB)'))
        : h(
            'div',
            { class: 'row' },
            h('button', { id: 'connect-usb', onclick: () => void connectUsb(), disabled: busy || s.phase === 'connecting' }, 'Connect over USB'),
            h('button', { id: 'connect-ble', onclick: () => void connectBluetooth(), disabled: busy || s.phase === 'connecting' || !navigator.bluetooth }, 'Bluetooth'),
          ),
      inTab() ? null : h('p', { class: 'muted' }, 'Bluetooth, and the first USB connection, open this wallet in a tab: Chrome draws its device chooser there.'),
    );
  } else if (s.phase === 'pairing') {
    lines.push(
      h('p', {}, 'Check that the board shows the same six digits, then press ALLOW on the board:'),
      h('p', { class: 'big mono', id: 'passkey' }, s.passkey ?? '…'),
      h('p', { class: 'muted' }, 'If they differ, deny it on the board: something is between this browser and the board.'),
      h('button', { onclick: () => ui.board.run('cancel') }, 'Cancel'),
    );
  } else if (s.phase === 'unlocking') {
    lines.push(h('p', { id: 'board-pin' }, s.waiting === 'pin' ? 'Enter your PIN on the board.' : 'Paired. Checking that the board is unlocked…'), h('button', { onclick: () => ui.board.run('cancel') }, 'Cancel'));
  } else if (s.phase === 'ready') {
    lines.push(
      h(
        'div',
        { class: 'row' },
        h('span', { class: 'grow', id: 'board-ready' }, `Connected: ${s.label ?? s.kind}${s.kind === 'ble' ? ' (keep this tab open)' : ''}`),
        h('button', { class: 'link', onclick: () => void guarded(null, () => ui.board.run('disconnect')), disabled: busy }, 'Disconnect'),
      ),
    );
  }
  if (s.error && s.phase === 'disconnected') lines.push(h('p', { class: 'notice bad' }, s.error));
  return h('section', { id: 'board' }, h('h2', {}, 'Board'), lines);
}

function boardImportSection() {
  if (!boardReady()) return null;
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
    h('h2', {}, 'Import from the board'),
    h('p', { class: 'muted' }, `The board gives its ${NETWORKS[net()]} viewing key for account 0; the spending keys stay on it.`),
    h('button', { class: 'primary', id: 'import-board', onclick: () => void importFromBoard(), disabled: !!ui.busy }, "Import the board's account"),
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
      : ui.boardState?.lastResult?.network === net() && !ui.signing
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
      ui.history.map((t) =>
        h(
          'li',
          {},
          h('span', {}, `${t.kind}${t.minedHeight ? ` @${t.minedHeight}` : ' (unmined)'}`),
          h('span', { class: 'mono' }, `${t.deltaZat < 0 ? '-' : '+'}${formatZec(t.amountZat)}`),
        ),
      ),
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
  const accts = ui.status?.accounts ?? [];
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
      ui.status?.accounts?.length ? h('p', { class: 'muted' }, `Wallet database: ${(ui.status.dbBytes / 1e6).toFixed(1)} MB on this browser's storage.`) : null,
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
        ? [boardSection(), boardImportSection(), importSection()]
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
    } else if (hs.network === net()) {
      await refresh();
    } else {
      await openNetwork();
    }
  });
  if (query.get('connect') === 'usb') ui.needUsbGrant = true;
  render();
  if (account() && !ui.syncing) void sync();
}

// `?bare`: the page without the popup's own logic, for test/e2e.mjs,
// which drives the engine itself from this extension page.
if (!query.has('bare')) main().catch((e) => {
  root.replaceChildren(h('p', { class: 'notice bad' }, `The wallet engine did not start: ${e?.message ?? e}`));
});
