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
| `src/web.rs` | The wasm-bindgen surface. It covers `Prover`, `Verifier`, `sign_prove_extract`, `lwd_info` and `fetch_and_scan`, plus `initThreadPool` in the `threads` builds. The gRPC calls go through `tonic-web-wasm-client`, which is gRPC-web over `fetch`. |
| `examples/native_*.rs` | The same work natively, for comparison. |
| `build.sh` | Wraps the wasm32 builds and wasm-bindgen. |
| `bench/node.mjs` | Runs the WASM in Node. Node is a single-threaded proxy, not the browser. |
| `bench/chromium.mjs`, `bench/ext/` | Load the spike as an unpacked MV3 extension in Playwright's Chromium. They run the bench in an extension tab or in an offscreen document, which is the context where the extension's engine will live. The manifest sets COOP/COEP, so the pages are `crossOriginIsolated`. |

The wallet database is missing. `zcash_client_sqlite` does not build for
wasm32 at this rev (see below), and `zcash_client_memory` is pre-Ironwood.
Without a database there is no `sync::run`, no `propose_transfer` and no
`create_pczt_from_proposal`. The spike exercises everything around them.

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
./build.sh sqlite           # fails, by design of the spike: see "What broke"

# native reference (cargo -j2; add --features multicore for rayon)
cargo run -j2 --release --example native_bench
cargo run -j2 --release --features engine --example native_flow

# Node (single-threaded proxy)
node bench/node.mjs pkg-engine

# Chromium MV3 extension (Playwright's Chromium 153 from ~/.cache/ms-playwright)
cd bench && npm install
SCAN=1000 node chromium.mjs tab:pkg-threads:8 offscreen:pkg-threads-engine:8 offscreen:pkg-engine
node chromium.mjs rawgrpc   # plain gRPC to testnet.zec.rocks with fetch(), no proxy
ONLY_LWD=1 LWD=https://testnet.zec.rocks node chromium.mjs offscreen:pkg-engine
```

Specs take the form `where:pkg[:threads]`, where `where` is `tab` or
`offscreen`. `LWD` sets the gRPC-web endpoint; it defaults to
`https://zcash-testnet.chainsafe.dev`. `SCAN` sets how many blocks below
the tip to fetch and trial-decrypt.

## What broke, and the fixes

| Problem | Status |
|---|---|
| `getrandom` 0.4 on wasm32 | Fixed with feature `wasm_js`. No `--cfg` is needed in 0.4. |
| `secp256k1-sys` needs a C compiler for wasm32 (`failed to find tool "clang"`) | Fixed with the NDK clang (`build.sh`). |
| `halo2_proofs`: "The multicore feature flag is not supported on wasm32 architectures without atomics" | Caused by `zcash_client_sqlite`'s default `multicore` feature. Fixed with `default-features = false`. Threads come from the `threads` build. |
| `zcash_client_sqlite`: `libsqlite3-sys 0.35` compiles bundled SQLite against libc, giving `sqlite3/sqlite3.c: fatal error: 'stdio.h' file not found` | **Not fixed.** rusqlite 0.37, pinned by the workspace and by `schemerz-rusqlite 0.370`, has no wasm32-unknown-unknown support. rusqlite 0.40 does, through `sqlite-wasm-rs`. |
| tonic native transport (hyper, tokio net, rustls) | Not used. `tonic-web-wasm-client` 0.8 replaces it; it needs a gRPC-web endpoint. |
| wasm-bindgen-rayon's default helper does `import('../../..')`, which only a bundler resolves | Fixed with feature `no-bundler`. |
| wasm-bindgen-rayon's `no-bundler` helper spawns workers from `blob:` URLs. MV3 CSP refuses that, so `initThreadPool` waits forever (WebZjs issue #141). | Fixed. `build.sh` patches the generated helper to spawn the packaged file instead. |
| Chromium 137+ ignores `--load-extension` | Fixed with `--disable-features=DisableLoadExtensionCommandLineSwitch` in `bench/chromium.mjs`. |

The measured numbers are in `docs/zcash/CHROME-ENGINE.md`.
