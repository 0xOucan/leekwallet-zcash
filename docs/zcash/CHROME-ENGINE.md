# The wallet engine in Chrome (Phase 9.1 spike)

Date: 2026-10-16. Code: `chrome/engine-spike/` (its README has the build
and bench commands).

The question: can the companion's whole job (sync, PCZT build, Orchard and
Ironwood proving, extraction, broadcast) run inside a Chrome MV3
extension, on our librustzcash rev `16f0723c` (NU6.3, Ironwood, v6
transactions)? The board only signs.

**Answer: yes, on our own librustzcash compiled to WASM, with threads,
in a dedicated worker, through a gRPC-web endpoint.** WebZjs is not an
option: it has no Ironwood.

---

## 1. Summary

| Question | Result |
|---|---|
| WebZjs | Pinned to ChainSafe's `librustzcash-nu61` fork (NU6.1). No NU6.3, no Ironwood, so it cannot see or spend most of today's shielded funds. Not usable. |
| Our librustzcash rev in WASM | Builds, with 7 fixes (section 3), including `zcash_client_sqlite`. The whole flow ran in an extension worker: init, `sync::run`, `propose_transfer`, `create_pczt_from_proposal`, sign (soft signer standing in for the board), prove, extract. |
| Threads | Required, not optional. Single-threaded, `sync::run` never finishes (section 3.1). |
| Where it runs | A dedicated worker started by the offscreen document. Pages may not block; the scanner and rayon need to. |
| gRPC from the browser | The public lightwalletd servers (zec.rocks) speak plain gRPC only. ChainSafe runs gRPC-web proxies for testnet and mainnet; we used the testnet one. A proxy-free path is possible but loses error status (section 4). |
| Proving, 2 actions | 9-10 s with 8 threads, 15-20 s with 4, about 57 s single-threaded. Native: 2.2 s multi-threaded, 4.8 s single. |
| Proving key | Built in 9-13 s with 8 threads (45 s single-threaded). Nothing to download. Once per session. |
| Sync | 8,500 blocks in 22 s; 50,000 blocks in 141 s (about 355 blocks/s), 8 threads. |
| Memory | 128-145 MB of WASM linear memory at peak, proving key included. |
| Storage | Wallet database 1.8 MB after 8,500 blocks, 4.6 MB after 50,000. |
| Binary | `pkg-threads-sqlite`: 13.0 MB raw, 4.3 MB gzip (no wasm-opt). |

Machine: Intel i7-1355U laptop (2 P-cores + 8 E-cores, 12 threads),
Playwright's Chromium 153, headless, the spike loaded as an unpacked MV3
extension with COOP/COEP (`crossOriginIsolated`, SharedArrayBuffer
available). Every number below is from Chromium unless it says Node or
native. The laptop was also running other work, and runs vary by up to
2x between repeats (the E-cores), so treat each figure as a range.

---

## 2. WebZjs

[ChainSafe/WebZjs](https://github.com/ChainSafe/WebZjs) is a Zcash
wallet in WASM. It is the engine behind the Zcash MetaMask Snap.

- **librustzcash pin.** Its
  [Cargo.toml](https://github.com/ChainSafe/WebZjs/blob/main/Cargo.toml)
  takes every Zcash crate from `ChainSafe/librustzcash-nu61`, branch
  `feat/snap-nu61` ("NU6.1 fixes"). The branch head is `01ad7597`, from
  2026-02-02. WebZjs `main` is at `a50df944` (2026-04-16).
- **Ironwood: none.** That fork's `zcash_protocol` has no `Nu6_3` and no
  Ironwood pool; it stops at NU6.1, plus the NU7 placeholder. It predates
  NU6.2's fixed Orchard circuit and NU6.3. Our rev needs v6 transactions
  and Ironwood bundles. Porting WebZjs to our rev would mean rewriting its
  storage too, because it uses `zcash_client_memory`, which is
  pre-Ironwood at our rev.
- **Transport.** gRPC-web through a proxy (`tonic-web-wasm-client`), the
  same as ours. Its README says unproxied servers "will NOT work", and
  ChainSafe hosts proxies.
- **Threads.** `initThreadPool` (wasm-bindgen-rayon), with sync in a web
  worker, as we found we need.
- **MV3.** Open [issue #141](https://github.com/ChainSafe/WebZjs/issues/141):
  `wallet.sync()` fails in a Chrome MV3 extension because of CSP. The
  cause is the blob: worker URLs that we hit and fixed (section 3).

What we take from WebZjs: the architecture (threads, sync in a worker,
gRPC-web) and its public proxies. The code we use is our own.

---

## 3. Our engine in WASM: what broke and what was fixed

Built from `chrome/engine-spike`: the zecz-core crates (`zcash_client_backend`
with `sync`, `zcash_client_sqlite`, `pczt`, `zcash_keys`, `orchard`
with `circuit`) at `16f0723c`, for `wasm32-unknown-unknown`.

| Problem | Fix |
|---|---|
| `getrandom` 0.4 on wasm32 | Feature `wasm_js`. |
| `secp256k1-sys` (transparent-inputs) needs a C compiler for wasm32 | Any LLVM clang; `build.sh` uses the Android NDK's. |
| halo2 `multicore` refuses wasm32 without atomics | `default-features = false` on `zcash_client_sqlite` and `zcash_primitives`; threads come from the `threads` builds. |
| rusqlite 0.37 (pinned by librustzcash) builds bundled SQLite against libc: `'stdio.h' file not found` | `patches/libsqlite3-sys`: a `[patch.crates-io]` stand-in that re-exports `sqlite-wasm-rs` (what rusqlite 0.40 uses). rusqlite 0.37 and `zcash_client_sqlite` then build unchanged. |
| `uuid` and `SystemTime::now()` on wasm32 | `uuid` feature `js`; a `Clock` over `Date.now()`. |
| Threads build: `sqlite3.o` not compiled with atomics | `-matomics -mbulk-memory` for the C code. |
| tonic 0.14.6 fails every gRPC-web stream ("missing grpc-status trailer") | tonic `=0.14.2` (host/ already uses it). |
| wasm-bindgen-rayon spawns workers from `blob:` URLs, which the MV3 CSP refuses; `initThreadPool` hangs (WebZjs #141) | `build.sh` patches the helper to spawn the packaged file. |
| **Single-threaded `sync::run` spins forever** | Threads, in a worker (3.1). |

### 3.1 Why threads are required

`zcash_client_backend` sends trial decryption to `rayon::spawn_fifo`
unconditionally, then blocks on a channel for the result
(`scan.rs`, `BatchRunner::collect_results`). Without threads, rayon's
fallback pool is the current thread, which is the one blocked, so the job
never runs. Sync stops at the first block that has shielded outputs, at
100 % CPU. Node showed it: the scan stalled after 5 empty blocks.

With the threads build the rayon workers run the job. The caller then
blocks in `Atomics.wait`, which browsers forbid on a page's main thread,
the offscreen document's included. So the engine lives in a dedicated
worker, which the offscreen document starts.

### 3.2 The full flow in an extension worker

`worker:pkg-threads-sqlite:8`: the abandon-art test account, birthday
8,500 blocks below the tip, against `zcash-testnet.chainsafe.dev`
(lightwalletd v0.5.4, testnet tip 4,510,377).

| Step | Time |
|---|---|
| Thread pool (8 workers) | 0.04-0.2 s |
| Init: schema, tree state, import UFVK | 0.8-1.5 s |
| `sync::run`, 8,501 blocks | 22.2 s (25.4 s with 4 threads) |
| `sync::run`, 50,001 blocks | 140.9 s |
| `propose_transfer` + `create_pczt_from_proposal` (0 zat to self, fee 10,000) | 0.14-0.6 s; PCZT 4,946 B |
| Proving key | 9.0-12.9 s |
| Sign (soft) + prove + extract, 2 Ironwood actions | 0.1 + 8.6-10.0 + 0.1 s; tx 9,166 B |
| Verifying key (only for the extractor's check) | 7-8.4 s |

The sync found the account's 10,000-zat Ironwood note. Spendable balance:
10,000. The PCZT carries a 0-zat payment to self, since the only note
cannot cover a positive amount plus the fee. Nothing was broadcast. The
extracted transaction is a valid v6 transaction from the extractor's
point of view, but broadcast (`SendTransaction`) was not exercised.

For comparison, the raw fetch + trial decryption of the same 8,500 blocks
outside the wallet took 0.6 s + 7.7 s (2.0 MB of compact blocks, 7,140
Orchard/Ironwood actions). Most of the sync time goes to the database and
the commitment trees, not the network or decryption. Natively, zecz syncs
about 1,000 blocks in under 3 s (journal, Phase 2).

---

## 4. gRPC from the browser

Browsers cannot read HTTP/2 trailers through `fetch`, and gRPC carries
its status in trailers. gRPC-web moves the status into the body.

| Endpoint | Speaks |
|---|---|
| `testnet.zec.rocks`, `zec.rocks` | plain gRPC (`application/grpc`) only |
| `zcash-testnet.chainsafe.dev`, `zcash-mainnet.chainsafe.dev` | gRPC-web (`application/grpc-web`) |
| `lightwalletd.testnet.electriccoin.co` | no answer |

gRPC-web through the ChainSafe testnet proxy: 1,000 blocks in 0.35 s,
8,500 blocks in 0.6 s.

**Plain gRPC without a proxy** (`node chromium.mjs rawgrpc`). With
`host_permissions`, an extension page may `fetch` zec.rocks with
`content-type: application/grpc`. It gets a 200 and the body holds the
messages, readable (GetLightdInfo 273 B, GetLatestBlock 44 B). But a
call to a missing method also answers 200 with an empty body: the
`grpc-status` is in a trailer that fetch cannot read. So a proxy-free
transport would work by treating "stream ended" as OK, and could not
tell an error from an empty answer.

For 9.2: the engine takes a gRPC-web URL. The default is ChainSafe's
proxy, and the user can set another, such as our own Envoy or
`grpcwebproxy` in front of a Zebra or Zaino lightwalletd. The proxy-free
transport stays an experiment: it removes a third party but loses
errors. A dead proxy is a liveness and privacy problem (it sees the
wallet's IP and which transactions it broadcasts), not a theft risk.
Everything it returns is checked against the commitment trees.

---

## 5. Proving

The NU6.3 Orchard/Ironwood circuit (Halo 2, no trusted setup, nothing to
download). Proof of a 2-action bundle (7,264 B), in seconds:

| Where | Threads | Proving key | Prove 2 actions | Prove 3 actions |
|---|---|---|---|---|
| Native (cargo --release) | 1 | 3.6 | 4.6-4.9 | 7.1 |
| Native (`multicore`) | 12 | 2.9 | 2.2 | 3.4 |
| Chromium, tab | 1 | 44.7 | 56-59 | 85 |
| Chromium, offscreen document | 1 | 44.4 | 59-60 | 89 |
| Chromium, offscreen document | 8 | 28.9 | 33-45 | 28 |
| Chromium, dedicated worker | 1 (threads build) | 49.4 | 32-47 | 39 |
| Chromium, dedicated worker | 4 | 16.7-31.1 | 16-20 | 25-31 |
| Chromium, dedicated worker | 8 | 9.0-19.1 | 9-20 | 13-43 |

- Single-threaded WASM is about 12x slower than native, because the
  pasta field arithmetic has no 64x64->128 multiply in WASM.
- 8 threads in a worker bring a send to about 10 s after the key is
  built, against 2.2 s native.
- The offscreen document's main thread joins rayon's work and is slower
  than a worker. Another reason to put the engine in a worker.
- Caching the proving key across sessions was not tried. Build it in
  the background when the wallet unlocks, so it is ready by the time the
  user confirms on the board.

---

## 6. Memory, size, storage

- **Memory:** 127-145 MB of WASM linear memory at peak (proving key plus
  wallet). The JS heap is under 10 MB.
- **Binary:** raw / gzip, built by `build.sh` without wasm-opt:
  - prover only: 4.1 / 0.85 MB;
  - engine (prover + pczt + gRPC-web): 7.3 / 2.6 MB;
  - engine + sqlite: 12.8 / 4.3 MB;
  - `threads-sqlite`: 13.0 / 4.3 MB.

  A CRX is a zip, so the download is about the gzip figure.
- **Storage:** the database here is SQLite in memory. After 8,500
  blocks it was 1.8 MB, after 50,000 blocks 4.6 MB. That is
  well within an extension's quota (`unlimitedStorage` lifts it anyway).
  Persistence is not tested yet. `sqlite-wasm-rs` has an OPFS VFS, and
  OPFS sync access handles work only in dedicated workers, which is
  where the engine already runs.

---

## 7. Recommendation

Build the extension's engine from our own librustzcash rev compiled to
WASM, as in `chrome/engine-spike`, not from WebZjs:

- an offscreen document (Web Serial and Web Bluetooth need a page),
  which starts a **dedicated worker** that holds the WASM engine;
- the **threads build** (atomics, wasm-bindgen-rayon with the CSP patch),
  with COOP/COEP in the manifest;
- **zcash_client_sqlite** through the `libsqlite3-sys` stand-in, on
  OPFS;
- **gRPC-web** to a configurable endpoint, ChainSafe's proxy by default;
- the zecz-core flow unchanged: the same calls the desktop and the CLI
  make, so one code path serves all three.

**Risks:**
1. **The `libsqlite3-sys` stand-in** relies on `sqlite-wasm-rs` matching
   the bindings rusqlite 0.37 expects. It builds and runs the whole flow,
   but it is a patch to a sys crate. It goes away when librustzcash moves
   to rusqlite 0.40.
2. **The gRPC-web proxy** is a third party, for privacy and liveness. We
   need a fallback endpoint we run ourselves. Broadcast through it is not
   tested yet.
3. **Proving time.** About 10 s on an 8-thread laptop, up to a minute on
   a single core. Low-end machines will be slow. Start the key at
   unlock, show progress, and keep native messaging to `zecz` as the
   fallback for users who want native speed.
4. **The single-threaded build is a dead end** (3.1). Without
   `crossOriginIsolated` (COOP/COEP) there is no wallet. Every extension
   page that hosts the engine must keep them.
5. **MV3 lifetime.** The offscreen document must stay open for the whole
   sync. The service worker was stopped mid-run in the spike. Keep no
   state in it.
6. **Persistence and long syncs:** OPFS, resume after a closed browser,
   and a sync from an old birthday are untested. At 355 blocks/s, a
   year of 25 s blocks (about 1.26 M) would take about an hour, if the
   rate held.
7. **Bundle size:** 4.3 MB gzip, and wasm-opt has not been tried.

---

## 8. Phase 9 plan, revised

- **9.1 Engine spike: done.** This document.
- **9.2 Engine package and software wallets (L, was M).**
  - First, turn the spike into `chrome/engine`: the worker, a message API
    (open, sync with progress, balances, propose, finish, broadcast), and
    sqlite on OPFS with persistence across restarts.
  - Test `SendTransaction` through the gRPC-web proxy with a funded
    testnet account, signed by the soft signer.
  - Then create or restore a software wallet with a passphrase, encrypted
    with WebCrypto (Argon2id in WASM), with auto-lock and several
    accounts.
  - Done when: a testnet send from a software wallet, broadcast from the
    extension.
- **9.3 The board (M).** Web Serial and Web Bluetooth (no QR through the
  webcam: the extension uses USB and Bluetooth only). The engine hands the PCZT to the device code and applies
  the `.zsig` with the same `PcztSignatures` path the other companions use.
- **9.4 Dapp interface (M)**, **9.5 NEAR Intents (M)**, **9.6 Store
  release (S):** unchanged. 9.6 adds the proxy disclosure to the privacy
  policy.
- **Fallback, kept open:** a native messaging host running `zecz`, for
  native proving speed or when the proxy is not acceptable. Not planned
  unless 9.2 shows a blocker.
