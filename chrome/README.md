# LeekWallet Zcash in Chrome

The Chrome extension (roadmap Phase 9). The board signs; everything else
(sync, PCZT, proving, extraction, broadcast) runs in the browser, on our
own librustzcash rev compiled to WASM. Why this architecture, and the
numbers behind it: [docs/zcash/CHROME-ENGINE.md](../docs/zcash/CHROME-ENGINE.md).

| Path | What it is |
|---|---|
| `engine/` | The wallet engine: a Rust crate built for `wasm32-unknown-unknown` with threads. zecz-core's flow over `zcash_client_sqlite` on OPFS and gRPC-web. |
| `extension/` | The MV3 extension: service worker, offscreen document, engine worker, popup. Plain ES modules, no bundler. |
| `engine-spike/` | The 9.1 spike, kept for its benchmarks. |

Status (9.2): watch-only and device accounts, sync, balances, addresses,
and the whole send path in the engine's API (propose, apply a `.zsig`,
prove, extract, broadcast). No board link yet (9.3) and no software
wallets yet (9.4); the popup has no send screen until then.

## How it fits together

```
popup.html ──port "engine"──> offscreen.html ──postMessage──> engine-worker.js
 (closes on blur)              (lives until the browser closes)  (dedicated worker)
                                                                   │ WASM engine (threads:
service worker: only creates the offscreen document                │ wasm-bindgen-rayon pool)
                                                                   ├─ OPFS: /<net>/wallet.sqlite, /<net>/leek.sqlite
                                                                   └─ fetch: gRPC-web proxy -> lightwalletd
```

- **Worker, not page:** the threads build blocks in `Atomics.wait` (rayon
  trial decryption, proving), which pages may not do, and OPFS sync access
  handles exist only in dedicated workers.
- **Cross-origin isolation:** the manifest sets COOP `same-origin` and
  COEP `require-corp`, so extension pages have `SharedArrayBuffer`.
  Without it there are no threads, and without threads `sync::run` never
  finishes (CHROME-ENGINE.md 3.1). `build.mjs` refuses a manifest without
  them.
- **Storage:** SQLite through `sqlite-wasm-vfs`'s OPFS sync access handle
  pool, in the extension's origin. It survives reloads, updates and
  browser restarts, and goes with an uninstall. A sync resumes from what
  the database recorded as scanned.
- **Network:** gRPC-web to a configurable endpoint. Default:
  ChainSafe's proxies (`zcash-testnet.chainsafe.dev`,
  `zcash-mainnet.chainsafe.dev`). The public lightwalletd servers speak
  plain gRPC only. Another endpoint (your own Envoy or `grpcwebproxy` in
  front of lightwalletd/Zaino) is set in the popup's settings, which asks
  for its host permission.
- **Shared code:** the engine compiles `host/leek-device/src/zsig.rs` (the
  `.zsig` v1/v2 format) and `host/zecz-core/src/{authorize,keys,soft_signer}.rs`
  unchanged, through `#[path]`. `host/zecz-core/src/wallet.rs` is bound to
  `std::fs`, tonic's HTTP/2 transport and tokio, so `engine/src/wallet.rs`
  restates its operations over OPFS and gRPC-web, naming the zecz-core
  function each one follows.

## Build

Prerequisites (once):

```sh
rustup target add wasm32-unknown-unknown
rustup component add rust-src                 # the threads build rebuilds std
cargo install -j2 wasm-bindgen-cli --version 0.2.129 --locked   # = Cargo.lock
```

and a clang that targets wasm32 for the C code (secp256k1, SQLite): the
Android NDK's (`$NDK_BIN`, the default) or any LLVM clang
(`CC_wasm32_unknown_unknown`, `AR_wasm32_unknown_unknown`). The threads
build uses `-Z build-std` on stable through `RUSTC_BOOTSTRAP=1`.

```sh
chrome/engine/build.sh            # -> chrome/engine/pkg/ (JOBS=2 by default)
cd chrome/extension
npm install                       # playwright-core, for the tests only
node build.mjs                    # -> chrome/extension/dist/
```

The release build takes a while (fat LTO, one codegen unit, std rebuilt):
about 12 minutes from scratch on an i7-1355U laptop with `-j2`, and still
about 10 after a one-line change, since the link-time optimisation redoes
the whole binary. `cargo check --target wasm32-unknown-unknown` (with the
same `CC_wasm32_unknown_unknown`) is the quick loop. The engine is 13.9 MB
of WASM.

## Load it

`chrome://extensions` → Developer mode → Load unpacked →
`chrome/extension/dist`. Open the toolbar popup: pick Testnet or Mainnet,
paste a UFVK (from the board: `zecz` prints it, or the desktop app), set
the birthday height, and it syncs. Closing the popup does not stop the
sync. The popup can also be opened as a tab:
`chrome-extension://<id>/popup.html?tab`.

## Test

```sh
cd chrome/extension
node test/e2e.mjs                  # headless Chromium (Playwright's, ~/.cache/ms-playwright)
BROADCAST=1 node test/e2e.mjs      # opt-in: also sends 1,000 zat to itself and broadcasts
```

`e2e.mjs` loads `dist/`, imports the abandon-art testnet account 0 UFVK
from `vectors/keys.json` as a device account (birthday `BIRTHDAY`, default
4,501,000), stops and resumes a sync, reloads the extension in the middle
of a sync, finishes it, reads balances, addresses and history, then
reloads the extension and restarts the browser and checks the wallet came
back from OPFS. It reloads through `chrome://extensions` with developer
mode on (Chromium disables a reloaded unpacked extension otherwise). The
broadcast run needs funds on that public account (its
keys are test vectors, so anyone may spend them) and signs with the dev
soft signer. Env: `LWD` (endpoint), `HEADED=1`, `KEEP=1`, `VERBOSE=1`.

## The engine's message API

Pages call the engine through `extension/src/engine-client.js`:

```js
const engine = await EngineClient.connect();
await engine.ready();
await engine.call('open', { network: 'test' });
engine.onEvent((ev) => console.log(ev));      // progress events
const r = await engine.call('sync', {});
```

Each call is `{ id, method, params }` over the port, answered by
`{ id, result }` or `{ id, error }`. Calls run one at a time in the
engine, in order (a sync holds it until it ends); `cancelSync`,
`workerState` and `hostState` are answered at once. Bytes are hex. Zatoshi
amounts are numbers. `account` is an account UUID and defaults to the
wallet's first account.

| Method | Params | Result |
|---|---|---|
| `hostState` | | `{ ready, info, fatal, busy, network, server, lastSync, lastSyncResult }` (the offscreen document's view) |
| `engineInfo` | | `{ apiVersion, engineVersion, librustzcash, threads, opfsFiles }` |
| `defaultServer` | `network` | the default gRPC-web URL |
| `tip` | `server?` or `network?` | `{ server, chainName, height, consensusBranchId, vendor, version }` |
| `open` | `network` (`test`/`main`), `server?` | status (below) plus `existed`. Closes the wallet open before. |
| `status` | | `{ open, network, server, accounts, sync, dbBytes, leekDbBytes }` |
| `close` | | `{ open: false }` |
| `setServer` | `server` | `{ server }` |
| `wipe` | `network` | `{ removed }`: deletes that network's files |
| `accounts` | | `[ { uuid, name, birthday, kind: "device"\|"viewOnly", origin, shortFingerprint, ufvk } ]` |
| `importAccount` | `ufvk`, `birthday?` (none: chain tip), `name?`, `origin?: { seedFingerprint, accountIndex }` | `{ imported, account, addresses }`. With `origin`, a device account (zecz `AccountSpec::device`); without, view-only. The same UFVK again: `imported: false`. |
| `addresses` | `account?` | `{ unified, diversifierIndex, transparent, orchardOnly }` |
| `nextAddress` | `account?` | `{ unified, diversifierIndex, transparent }`: a fresh diversified address (rotation) |
| `checkAddress` | `address`, `network?` | `{ kind: "unified"\|"transparent" }` |
| `sync` | `batch?` (1000) | `{ cancelled, tip, scannedBlocks, elapsedMs, status }`. Events below. |
| `cancelSync` | | `{ cancelling }`: the sync stops after its batch, resolving `cancelled: true` |
| `syncStatus` | | `{ chainTip, fullyScanned, progressNum, progressDen, synced }` or null |
| `balances` | `account?` | `{ account, chainTip, fullyScanned, synced, balances: { orchard, ironwood, transparent: { total, spendable, pending }, total, spendable } }` |
| `history` | `account?`, `limit?` (50) | `[ { txid, minedHeight, blockTime, expiryHeight, expiredUnmined, kind, deltaZat, amountZat, feeZat, memos, recipients } ]` |
| `proposeSend` | `account?`, `to`, `amountZat`, `memo?`, `compact?` (true) | `{ account, targetHeight, payments, change, feeZat, signerView, compact, sha256 }`. The full PCZT stays in the engine under `sha256` (hash of `signerView`). |
| `proposeShield` | `account?`, `compact?` | the same, for shielding the transparent coins |
| `pending` | | the proposals kept, newest first, with `stage`: unsigned, signed, proven, extracted |
| `applyZsig` | `signerView`, `zsig` | `{ sha256, signatures }`: checks the `.zsig` (v1 or v2) signs exactly `signerView`, that the view matches the kept PCZT (same sighash), and applies each signature (verified) |
| `prove` | `sha256` | `{ proveMs }`. Events below. Builds the proving key the first time (seconds). |
| `extract` | `sha256` | `{ txid, size, raw }`: stored in the wallet and kept for broadcast |
| `broadcast` | `txid` | `{ txid, broadcast: true }` (SendTransaction through the endpoint) |
| `finish` | `signerView`, `zsig`, `broadcast?` | applyZsig + prove + extract (+ broadcast), as `zecz finish` |
| `warmProver` | | `{ built, ms }`: builds the proving key ahead of a send |
| `signSoft` | `signerView`, `mnemonic`, `accountIndex?` | `{ zsig }`. DEV ONLY, testnet only: zecz's soft signer, standing in for the board until 9.3 |
| `devAccountSpec` | `mnemonic`, `accountIndex?` | `{ ufvk, origin }`. DEV ONLY, testnet only |

Events (`{ event }` to every connected page):

- `{ event: "sync", phase: "started", server, tip, fullyScanned }`
- `{ event: "sync", phase: "scanned", start, end, tip, scannedBlocks, elapsedMs }`
- `{ event: "import", phase: "treeState", height, server }`
- `{ event: "prove", phase: "loadingKey" | "proving", pool, circuit?, elapsedMs }`
- `{ event: "finish", phase: "signaturesApplied" | "stored", ... }`

The engine itself (`Engine.call(method, paramsJson, onEvent)` in
`engine/src/web.rs`, dispatched in `engine/src/api.rs`) takes the same
methods; the worker adds `cancelSync` and `workerState`, the offscreen
document `hostState`.

## Not yet

- 9.3: the board over Web Serial / Web Bluetooth / QR, verify-address on
  the device; the send screen.
- 9.4: software wallets (create/restore, passphrase, WebCrypto), several
  accounts in the popup.
- The proving key is not cached across sessions; wasm-opt is used only
  when installed.
