# LeekWallet Zcash desktop

The desktop app for Linux, Windows and macOS (roadmap Phase 8). Tauri v2:
a Rust backend over the `zecz` wallet library (`host/zecz-core`) and a
plain TypeScript frontend with LeekWallet's look.

This is the 8.1 skeleton. It imports a unified full viewing key as a
watch-only account, syncs it from lightwalletd with a progress bar, and
shows the balance per pool (Orchard, Ironwood, transparent) and the unified
address. It cannot spend; software wallets (8.2) and hardware accounts
(8.3) come next.

## Layout

```
desktop/
  index.html, src/        the frontend (main.ts, styles.css, tokens.css)
  vite.config.js          dev server on :1421, serving the shipping CSP
  src-tauri/
    src/backend.rs        blocking calls over zecz::wallet, serializable results
    src/lib.rs            the Tauri commands; work runs on worker threads
    tests/backend.rs      backend tests (one #[ignore]d, on testnet)
    tauri.conf.json, capabilities/, icons/
```

`src-tauri` is its own Cargo workspace (not a member of `host/`), so the
Tauri dependency tree stays out of the lock file the CLI and the Android
library build from. It depends on `host/zecz-core` by path.

Commands: `profile_status`, `init_watch_only(ufvk, birthday)`, `sync`
(emits `sync://progress`), `balances`, `addresses`, `tip`.

## Where the wallet lives

`<app data dir>/wallets/<test|main>`, a zecz data directory (the same
format as `zecz --data-dir`):

- Linux: `~/.local/share/com.leekwallet.zcash/wallets/test`
- macOS: `~/Library/Application Support/com.leekwallet.zcash/wallets/test`
- Windows: `%APPDATA%\com.leekwallet.zcash\wallets\test`

Testnet by default. `LEEKZ_NETWORK=main` selects mainnet and
`LEEKZ_SERVER=https://host:port` another lightwalletd; there is no switch
in the UI yet.

## Requirements

- Rust (stable) and Node 22+ with pnpm (`corepack enable`; the version is
  pinned in `package.json`).
- Linux: `libwebkit2gtk-4.1-dev libayatana-appindicator3-dev librsvg2-dev
  libudev-dev libssl-dev patchelf` (Debian/Ubuntu names).
- Windows: WebView2 (preinstalled on Windows 10/11) and the MSVC build tools.
- macOS: the Xcode command line tools.

## Run in development

```sh
cd desktop
pnpm install
pnpm tauri dev
```

The first build compiles the librustzcash stack and takes several minutes.
Dependencies are always built optimized (`[profile.dev.package."*"]`),
since scanning is unusably slow without it. On a machine with little RAM,
limit the jobs: `CARGO_BUILD_JOBS=2 pnpm tauri dev`.

## Build

```sh
cd desktop
pnpm tauri build            # release bundles in src-tauri/target/release/bundle/
pnpm tauri build --debug    # quicker, unoptimized app crate
```

Linux produces a .deb and an AppImage, Windows an MSI, macOS a .app and
a .dmg (unsigned; signing and notarization are 8.7).

## Test

`tauri::generate_context!` embeds `dist/`, so build the frontend first:

```sh
cd desktop
pnpm build
cd src-tauri
cargo test                                   # offline
cargo test --test backend -- --ignored       # import + sync on testnet
```

The testnet test imports the public test account (the 24-word
"abandon ... art" mnemonic, UFVK from `vectors/keys.json`) as watch-only
about 2,500 blocks below the tip, syncs it through
`https://testnet.zec.rocks:443`, and checks the progress events, balances
and addresses.

CI (`.github/workflows/desktop.yml`) builds the app on Ubuntu, Windows and
macOS and runs the offline tests and `host/` tests on Ubuntu.
