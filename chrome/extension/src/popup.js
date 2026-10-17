// The popup: network switch, import a viewing key, sync progress,
// balances per pool, receive address (with rotation), recent history.
//
// It owns nothing: every answer comes from the engine through
// EngineClient, and the few choices it remembers (network, endpoint,
// account) are in chrome.storage.local. Text from the chain (memos,
// addresses) only ever goes into textContent.

import { EngineClient } from './engine-client.js';

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
    h(
      'p',
      { class: 'notice' },
      'Before receiving anything that matters, check this address on the device (from 9.3, the extension asks it to show the same one).',
    ),
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
    net() === 'main' ? h('p', { class: 'notice' }, 'Mainnet: real funds. This build is a preview; sending needs the board (9.3).') : null,
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
          syncSection(),
          receiveSection(),
          historySection(),
        ]
      : engineOk && ui.status
        ? importSection()
        : null,
    engineOk && ui.status ? settingsSection() : null,
  );
}

// --------------------------------------------------------------- start

async function main() {
  await loadPrefs();
  ui.engine = await EngineClient.connect();
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
  if (account() && !ui.syncing) void sync();
}

// `?bare`: the page without the popup's own logic, for test/e2e.mjs,
// which drives the engine itself from this extension page.
if (!query.has('bare')) main().catch((e) => {
  root.replaceChildren(h('p', { class: 'notice bad' }, `The wallet engine did not start: ${e?.message ?? e}`));
});
