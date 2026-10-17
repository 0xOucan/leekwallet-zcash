# engine-spike: the Zcash engine in a Chrome extension (Phase 9.1)

The question: can the companion's work run inside Chrome as WASM, on our
librustzcash rev (`16f0723c`, NU6.3, Ironwood, v6 transactions)? That work
is sync, PCZT build, Orchard/Ironwood proving, extraction and broadcast.
The board only signs.

The findings, numbers and recommendation are in
[docs/zcash/CHROME-ENGINE.md](../../docs/zcash/CHROME-ENGINE.md). This
README covers how to rebuild and rerun them.

## What is in here

| Path | What it does |
|---|---|
| `src/prove.rs` | Builds the NU6.3 proving key and proves an Ironwood (v3) bundle of N outputs. |
| `src/engine.rs` (feature `engine`) | Two pieces. First, sign / prove / extract of the repo's real vector `vectors/pczt/iw_send_shielded.pczt`. It uses the dev soft signer for the board's part, then `pczt` Prover and TransactionExtractor. Second, `scan_block` trial decryption of compact blocks for a UFVK. |
| `src/wallet.rs` (feature `sqlite`) | zecz-core's wallet flow: `zcash_client_sqlite` (in memory), `sync::run` over gRPC-web, then `propose_transfer` and `create_pczt_from_proposal` of a send to self. Nothing is broadcast. |
| `src/web.rs` | The wasm-bindgen surface. It covers `Prover`, `Verifier`, `sign_prove_extract`, `lwd_info`, `fetch_and_scan` and `wallet_run`, plus `initThreadPool` in the `threads` builds. The gRPC calls go through `tonic-web-wasm-client`, which is gRPC-web over `fetch`. |
| `patches/libsqlite3-sys` | A stand-in for libsqlite3-sys 0.35 that re-exports `sqlite-wasm-rs`, so rusqlite 0.37 builds for wasm32 (see below). |
| `examples/native_*.rs` | The same work natively, for comparison. |
| `build.sh` | Wraps the wasm32 builds and wasm-bindgen. |
| `bench/node.mjs` | Runs the WASM in Node. Node is a single-threaded proxy, not the browser. |
| `bench/node-wallet.mjs` | The single-threaded `pkg-sqlite` wallet flow in Node. It reproduces the hang described below. |
| `bench/chromium.mjs`, `bench/ext/` | Load the spike as an unpacked MV3 extension in Playwright's Chromium. They run the bench in an extension tab, in an offscreen document, or in a dedicated worker started by an offscreen document (`worker`), which is where the extension's engine will live. The manifest sets COOP/COEP, so the pages are `crossOriginIsolated`. |

The wallet database is in memory: persistence (sqlite-wasm-rs's OPFS VFS,
from a worker) is not exercised.

## Prerequisites

```sh
rustup target add wasm32-unknown-unknown
rustup component add rust-src                        # for the threads builds (build-std)
cargo install -j2 wasm-bindgen-cli --version 0.2.129 --locked   # must match Cargo.lock
```

Some dependencies contain C code: `secp256k1-sys`, through
`transparent-inputs`. Building it needs a clang that targets wasm32. Any
LLVM clang works. `build.sh` uses the Android NDK's by default
(`$HOME/Android/Sdk/ndk/27.2.12479018/.../bin/clang`). To use another one,
override `NDK_BIN` or `CC_wasm32_unknown_unknown`/`AR_wasm32_unknown_unknown`.
The threads builds use `-Z build-std` on the stable toolchain through
`RUSTC_BOOTSTRAP=1`, so no nightly install is needed.

## Commands

```sh
cd chrome/engine-spike
./build.sh prover           # pkg-prover/          single-threaded prover
./build.sh engine           # pkg-engine/          + pczt, client_backend, gRPC-web
./build.sh threads          # pkg-threads/         prover with rayon (atomics, shared memory)
./build.sh threads-engine   # pkg-threads-engine/  both
./build.sh sqlite           # pkg-sqlite/          + zcash_client_sqlite and the wallet flow
./build.sh threads-sqlite   # pkg-threads-sqlite/  the same with rayon: the one that syncs

# native reference (cargo -j2; add --features multicore for rayon)
cargo run -j2 --release --example native_bench
cargo run -j2 --release --features engine --example native_flow

# Node (single-threaded proxy)
node bench/node.mjs pkg-engine

# Chromium MV3 extension (Playwright's Chromium 153 from ~/.cache/ms-playwright)
cd bench && npm install
SCAN=1000 node chromium.mjs tab:pkg-prover offscreen:pkg-engine offscreen:pkg-threads:8 worker:pkg-threads:8
# the whole flow: birthday 8,500 blocks back (finds the abandon-art 10,000-zat
# Ironwood note), sync, a 0-zat send to self, sign/prove/extract
SEND=0 SCAN=8500 node chromium.mjs worker:pkg-threads-sqlite:8
node chromium.mjs rawgrpc   # plain gRPC to testnet.zec.rocks with fetch(), no proxy
ONLY_LWD=1 LWD=https://testnet.zec.rocks node chromium.mjs offscreen:pkg-engine
```

Specs take the form `where:pkg[:threads]`, where `where` is `tab`,
`offscreen` or `worker`. `LWD` sets the gRPC-web endpoint; it defaults to
`https://zcash-testnet.chainsafe.dev`. `SCAN` sets how many blocks below
the tip to fetch and trial-decrypt; for the sqlite pkgs it is the wallet
birthday's distance below the tip. `SEND` is what the wallet sends to
itself (default 1,000 zat).

## What broke, and the fixes

| Problem | Status |
|---|---|
| `getrandom` 0.4 on wasm32 | Fixed with feature `wasm_js`. No `--cfg` is needed in 0.4. |
| `secp256k1-sys` needs a C compiler for wasm32 (`failed to find tool "clang"`) | Fixed with the NDK clang (`build.sh`). |
| `halo2_proofs`: "The multicore feature flag is not supported on wasm32 architectures without atomics" | Caused by `zcash_client_sqlite`'s default `multicore` feature. Fixed with `default-features = false`. Threads come from the `threads` build. |
| `zcash_client_sqlite`: `libsqlite3-sys 0.35` compiles bundled SQLite against libc, giving `sqlite3/sqlite3.c: fatal error: 'stdio.h' file not found` | Fixed. rusqlite 0.37 (pinned by librustzcash and `schemerz-rusqlite`) has no wasm32 support, but rusqlite 0.40 gets it from `sqlite-wasm-rs`, which exposes the same C API. `patches/libsqlite3-sys` re-exports it under the old name through `[patch.crates-io]`. |
| `uuid` needs a randomness source on wasm32 | Fixed with feature `js`. |
| `SystemTime::now()` panics on wasm32 | `wallet.rs` gives `WalletDb` a `Clock` over `Date.now()`. |
| Threads build: `--shared-memory is disallowed by sqlite3.o because it was not compiled with 'atomics' or 'bulk-memory'` | Fixed. `build.sh` passes `-matomics -mbulk-memory` to the C compiler. |
| Single-threaded `sync::run` spins forever at 100 % CPU on the first block with shielded outputs. `zcash_client_backend` hands trial decryption to `rayon::spawn_fifo` and blocks on a channel; with no threads nothing ever runs the job. | **Inherent.** Use the threads build. The wait then blocks the calling thread, which a page may not do (`Atomics.wait`), so the engine runs in a dedicated worker. |
| tonic native transport (hyper, tokio net, rustls) | Not used. `tonic-web-wasm-client` 0.8 replaces it; it needs a gRPC-web endpoint. |
| wasm-bindgen-rayon's default helper does `import('../../..')`, which only a bundler resolves | Fixed with feature `no-bundler`. |
| wasm-bindgen-rayon's `no-bundler` helper spawns workers from `blob:` URLs. MV3 CSP refuses that, so `initThreadPool` waits forever (WebZjs issue #141). | Fixed. `build.sh` patches the generated helper to spawn the packaged file instead. |
| Chromium 137+ ignores `--load-extension` | Fixed with `--disable-features=DisableLoadExtensionCommandLineSwitch` in `bench/chromium.mjs`. |

The measured numbers are in `docs/zcash/CHROME-ENGINE.md`.
