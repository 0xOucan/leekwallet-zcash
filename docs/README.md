# Documentation

Website: [zcash.leekwallet.xyz](https://zcash.leekwallet.xyz) · Demo: [2-minute video](https://www.youtube.com/watch?v=8F6_8AhsnAA) · X: [@gmLeekWallet](https://x.com/gmLeekWallet) · [@0xOucan](https://x.com/0xOucan)

| Document | What it covers |
|---|---|
| [zcash/ROADMAP.md](zcash/ROADMAP.md) | The plan, phase by phase, each step with a "done when" check and its status |
| [JOURNAL.md](JOURNAL.md) | What was done at each step, what broke, what was measured, and the transactions signed on the board |
| [zcash/ANALYSIS.md](zcash/ANALYSIS.md) | Why: the starting point, options and decisions |
| [zcash/PLAN.md](zcash/PLAN.md) | Architecture: board, companions, formats |
| [zcash/FORMATS.md](zcash/FORMATS.md) | `.zsig` (v1, v2), `.zkv` seed cards, the SD card layout, the animated QR (`ur:leek-*`) |
| [zcash/QR-BENCH.md](zcash/QR-BENCH.md) | QR signing: the settings that read fastest, phones, View key, Check address, the transactions signed by QR |
| [zcash/MAINNET.md](zcash/MAINNET.md) | Checklist for the first mainnet tests with real funds |
| [zcash/CHROME-ENGINE.md](zcash/CHROME-ENGINE.md) | The browser engine study: numbers and the chosen design |
| [zcash/zip-draft-app-storage-keys.md](zcash/zip-draft-app-storage-keys.md) | Draft, not submitted: a ZIP for per-app storage keys that the board derives (ZIP 32 Registered derivation) and releases after approval, so companion apps keep their data encrypted at rest |
| [TOOLCHAIN.md](TOOLCHAIN.md) | Toolchains and versions |
| [../firmware/docs/PROTOCOL.md](../firmware/docs/PROTOCOL.md) | The device protocol: pairing, session, every call |
| [../firmware/docs/WIRING-S3CAM.md](../firmware/docs/WIRING-S3CAM.md) | Wiring the board, screen, encoder and buttons |

## Product READMEs

The products and what each one is: [Ecosystem](../README.md#ecosystem) in
the top README; which wallet reaches the board over USB, Bluetooth or QR:
[Wallets and links](../README.md#wallets-and-links).

| Product | README |
|---|---|
| Board firmware (`firmware/`, upstream LeekWallet's README with a Zcash note) | [firmware/README.md](../firmware/README.md) |
| Zcash LeekWallet app (`app/`, desktop and Android, NEAR Intents swaps) | [app/README.md](../app/README.md), Android build: [ANDROID.md](../app/ANDROID.md) |
| Chrome extension (`chrome/`) | [chrome/README.md](../chrome/README.md) |
| LeekWallet Zcash for Android (Zodl fork, own repo) | [leekwallet-zcash-android](https://github.com/0xOucan/leekwallet-zcash-android#readme) |
| Zingo fork (own repos; built on Zingo Labs' PCZT work, not an official Zingo release) | [zingo-mobile `leekwallet-qr`](https://github.com/0xOucan/zingo-mobile/tree/leekwallet-qr), [zingolib `leekwallet-signer`](https://github.com/0xOucan/zingolib/tree/leekwallet-signer) |
| LeekWallet SDK (`leek-zcash`, `leek-zcash-ffi`) | [leek-zcash](../host/leek-zcash/README.md), [leek-zcash-ffi](../host/leek-zcash-ffi/README.md) |
| `zecz` CLI | [host/zecz-cli/README.md](../host/zecz-cli/README.md) |
| Host crates | [host/README.md](../host/README.md): [zecz-core](../host/zecz-core/README.md), [leek-device](../host/leek-device/README.md), [leek-android](../host/leek-android/README.md) |
| Simulator and firmware tests (zdev) | [firmware/sim/README.md](../firmware/sim/README.md) |
| Test vectors | [vectors/README.md](../vectors/README.md) |
