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

Status (9.3): watch-only and device accounts, sync, balances, addresses,
and the board over USB (Web Serial) and Bluetooth (Web Bluetooth): pair
with the six digits, import the board's account, check an address on its
screen, and send or shield signed on it, proven and broadcast here.
Tested against zdev (the firmware's protocol on a PTY), and on the real
board over USB in Brave 152: pairing, import and sync on testnet and
mainnet, an address confirmed on the board, and a mainnet send signed on
the board, proven and broadcast from the extension. Bluetooth is not yet
tried on the board. No software wallets yet (9.4), no QR yet.

## How it fits together

```
popup.html ──port "engine"──> offscreen.html ──postMessage──> engine-worker.js
 (closes on blur)              (lives until the browser closes)  (dedicated worker)
                                 │ BoardHost (USB)                 │ WASM engine (threads:
                                 └─ Web Serial ── board            │ wasm-bindgen-rayon pool)
popup.html?tab (a tab)                                             ├─ OPFS: /<net>/wallet.sqlite, /<net>/leek.sqlite
 └─ BoardHost (Bluetooth) ── Web Bluetooth ── board                └─ fetch: gRPC-web proxy -> lightwalletd
service worker: only creates the offscreen document
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

## The board

`extension/src/board/` is the board's protocol in plain ES modules, the
JavaScript twin of `host/leek-device/src/device.rs` (zecz, the desktop app
and Android use that one) and of LeekWallet's `session.ts`/`framing.ts`:

| File | What |
|---|---|
| `framing.js` | USB frames with the `LK` marker among console text; BLE frames in 1-byte-header chunks (MTU 23: Web Bluetooth does not expose the negotiated MTU) |
| `cbor.js` | The protocol's CBOR subset, canonical |
| `session.js` | v2 pairing crypto: X25519, SHA-256 and HKDF from WebCrypto (Chrome 133+), ChaCha20-Poly1305 from `@noble/ciphers` (vendored at build time from the pinned npm package) |
| `device.js` | `hello`/`helloReveal` with the commitment check, the passkey, waiting for ALLOW and the PIN, then `zGetFeatures`, `zGetAccount`, `zSetAccountMeta` (with `network`), `zShowAddress` (with `ua`: any rotated address of the account), `zSignBegin/Chunk/Finish` |
| `zsig.js` | Writes the `.zsig` (v1, or v2 with transparent signatures) the engine's `finish` applies |
| `serial-link.js`, `ble-link.js` | The two links. Serial: DTR/RTS low after open, boot noise drained (LeekWallet's lessons) |
| `host.js` | `BoardHost`: connect, import the account into the engine (the board's birthday for this network, or ask and record it), verify an address, sign and finish |

Where it runs, and why:

- **USB: the offscreen document.** Web Serial needs a document, and a
  signature must outlive the popup (it closes on blur). `requestPort()`
  needs a click in a tab: the toolbar popup closes when Chrome's chooser
  takes focus, so the first USB connection opens the wallet in a tab once.
  The grant is kept for the extension's origin, so from then on the
  offscreen document's `getPorts()` finds the board (Espressif VID 0x303a).
  Its state reaches every page as `{ board }` on the engine port, and its
  operations are `board.<op>` calls on that port (`board.connect`,
  `board.importAccount`, `board.showAddress`, `board.signAndFinish`,
  `board.cancel`, `board.disconnect`, `board.state`).
- **Bluetooth: the tab that chose the device.** A `BluetoothDevice` cannot
  be handed to another document, `requestDevice()` needs a click in a tab,
  and `navigator.bluetooth.getDevices()` (to find it again elsewhere) is
  still behind a flag. So a Bluetooth board is driven from that tab, which
  stays open while it signs; the engine is still the offscreen one.

The popup: Connect over USB / Bluetooth, the six digits to compare (press
ALLOW on the board), "Enter your PIN on the board", Import the board's
account (account 0 of the network chosen in the header), Verify on the
board (the address shown, rotated or not), a send form (to, amount, memo)
and a Shield button when there are transparent funds. Both build a compact
proposal, show a review (payments, fee, change), require a real-funds tick
on mainnet, then sign on the board and prove, extract and broadcast in the
engine, showing the txid.

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
npm install                       # @noble/ciphers (vendored into dist), playwright-core (tests)
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
node --test test/board.test.mjs    # the board protocol in Node against zdev (no browser, no board)
node test/e2e.mjs                  # headless Chromium (Playwright's, ~/.cache/ms-playwright)
BROADCAST=1 node test/e2e.mjs      # opt-in: also sends 1,000 zat to itself and broadcasts
BROADCAST=board node test/e2e.mjs  # the same, signed by zdev through the board code
```

`board.test.mjs` builds zdev (`make -C firmware/sim build/zdev`), starts
it on a PTY with the abandon-art seed and talks to it as the extension
talks to a board: over the USB framing, and over Bluetooth through a fake
GATT server that chunks at MTU 23 as the board does. It checks the
passkey against the one zdev prints, every `z*` call against
`vectors/` (accounts on both networks, birthdays per network, address
check and refusal, compact and full sends, a shield with transparent
signatures and a version 2 `.zsig`, mainnet, refusals), a rejection on
the "device", and `BoardHost` with a stand-in engine. The `.zsig` writer
re-encodes `vectors/zsig/iw_send_shielded.zsig` byte for byte.

`e2e.mjs` also drives zdev from an extension page (step 5b) through a
bridge to its PTY: the same modules, with Chromium's WebCrypto.

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
| `signSoft` | `signerView`, `mnemonic`, `accountIndex?` | `{ zsig }`. DEV ONLY, testnet only: zecz's soft signer, for tests without a board |
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

- 9.3: Bluetooth on the real board (USB works); QR through the webcam.
  Account 0 only.
- 9.4: software wallets (create/restore, passphrase, WebCrypto), several
  accounts in the popup.
- The proving key is not cached across sessions; wasm-opt is used only
  when installed.
