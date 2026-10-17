# LeekWallet Zcash desktop

The desktop app for Linux, Windows and macOS (roadmap Phase 8). Tauri v2:
a Rust backend over the `zecz` wallet library (`host/zecz-core`) and a
plain TypeScript frontend with LeekWallet's look.

It imports an account from a LeekWallet over USB or Bluetooth (8.3), or a
unified full viewing key as a watch-only account (8.1). It syncs from
lightwalletd with a progress bar, shows the balance per pool (Orchard,
Ironwood, transparent) and the unified address with its QR code, rotates
to fresh addresses, verifies an address on the device, and sends: the
app builds the transaction, the LeekWallet reviews and signs it, the app
proves and broadcasts it. It shields transparent funds the same way,
lists the account's history, and keeps an address book (8.4). Software
wallets (8.2) come later.

## Layout

```
desktop/
  index.html, src/        the frontend (main.ts, styles.css, tokens.css)
  vite.config.js          dev server on :1421, serving the shipping CSP
  src-tauri/
    src/backend.rs        blocking calls over zecz::wallet, serializable results
    src/hardware.rs       the device flows: pair, import, verify, sign, finish
    src/link.rs           USB serial and Bluetooth LE links to the device
    src/contacts.rs       the address book (JSON in the app data dir)
    src/settings.rs       the network switch and servers (settings.json)
    src/lib.rs            the Tauri commands; work runs on worker threads
    tests/backend.rs      backend tests (one #[ignore]d, on testnet)
    tests/device_zdev.rs  the device flows against zdev (one #[ignore]d)
    tests/settings.rs     the network switch: what is saved, which wallet opens
    tauri.conf.json, capabilities/, icons/
```

`src-tauri` is its own Cargo workspace (not a member of `host/`), so the
Tauri dependency tree stays out of the lock file the CLI and the Android
library build from. It depends on `host/zecz-core` by path.

Commands:

- Network: `network_settings`, `set_network(network)` ("test" or
  "main"; refused while a wallet or device call runs),
  `set_server(server?)` (none: the default).
- Wallet: `profile_status`, `init_watch_only(ufvk, birthday)`, `sync`
  (emits `sync://progress`), `balances`, `addresses`, `next_address`,
  `qr_svg(text)`, `history`, `tip`.
- Contacts: `contacts_list`, `contact_save(name, address, replacing?)`,
  `contact_remove(name)`; each returns the book.
- Device: `list_ports` (USB serial ports, the board's Espressif VID 303a
  first), `ble_scan`, `device_pair(transport, port?, account?)` (emits the
  six digits on `device://passkey`), `device_session`,
  `device_disconnect`, `device_import(birthday?)`, `verify_address(ua)`,
  `propose_send(to, amount, memo?)` (amount in ZEC; builds the compact
  signer view), `propose_shield` (all transparent funds into the
  shielded pool) and `sign_and_send(proposalId, confirmMainnet?)`, which
  signs either and returns the txid (on mainnet only with
  `confirmMainnet: true`). One proposal is pending at a time: a new one
  replaces the last. Device flows report each step on `device://status`.

## Hardware accounts

The LeekWallet holds the seed. The app keeps a watch-only copy of the
account it exports (with its ZIP 32 origin, so its PCZTs name the key the
device signs with) and never sees a spending key. The flows are those of
`zecz device-init`, `device-show-address`, `send --compact`, `sign-device`
and `finish`:

1. **Connect.** Pick a link: a USB serial port, or Bluetooth (Scan
   Bluetooth, or "first LeekWallet found"). The device uses one link at a
   time; select the same one in its Settings. The app pairs (protocol v2,
   PROTOCOL.md §3) and shows a six-digit passkey: compare it with the
   device and press ALLOW there. If the device is locked it asks for its
   PIN on its own screen. The session stays open until Disconnect, a
   broken link, or the device locking.
2. **Import.** The device exports its account (zGetAccount). The birthday
   comes from the device when a companion recorded it there, or from the
   field; a birthday typed by the user is recorded on the device
   (zSetAccountMeta) for the next companion.
3. **Verify on device.** The receive card sends the address it shows; the
   device checks that it is one of its own and pages it for the user to
   compare. A foreign address is refused before the screen ("NOT from its
   wallet").
4. **Send.** To, amount and memo build a proposal; the review shows the
   payments, change and fee. "Sign on LeekWallet" uploads the compact
   PCZT (about 1.4 KB), the user reviews it on the device and ACCEPTs, then
   the app applies the signatures (each checked against its rk), proves,
   stores and broadcasts, and shows the txid.

5. **Shield.** When the transparent balance is above zero, the balance
   card offers "Shield transparent funds": the review shows the amount,
   the fee and the pool it goes into (Ironwood from NU6.3); the device
   shows each transparent coin and "Shield … into this wallet", and signs
   each coin with the key it re-derives (roadmap 10.1). When the balance
   does not cover the fee, or the fee would take all of it, the app shows
   zecz's refusal instead of a review.

Before verifying or signing, the app checks that the connected device
holds the wallet's seed (the ZIP 32 seed fingerprint), so a second board
is refused with both fingerprints rather than failing on the device.

One device call runs at a time. Errors from the device read as sentences
(rejected, timed out, locked, session ended, refused to sign with its
reason code), with the device's own message after them.

## Wallet screens

- **Receive.** The unified address (Orchard and P2PKH receivers) in
  groups of four and as a QR code (SVG drawn by the backend with the
  `qrcode` crate; black on white in either theme). "New address" rotates
  to a fresh diversified address, recorded in the wallet so what is paid
  to it is found: Orchard and P2PKH while the transparent gap limit (10
  unused addresses) allows, then Orchard only. Earlier addresses keep
  working; the app shows the default one again after a restart. "Verify
  on device" accepts any of them. The transparent address is shown
  apart, under a PUBLIC warning.
- **History.** The account's transactions from the wallet database
  (zcash_client_sqlite's `v_transactions` and `v_tx_outputs`), newest
  first with unmined ones on top: date and height, received, sent,
  shielded or moved between pools, amount, the fee when this wallet paid
  it, recipients (by contact name when known), readable memos, and the
  txid with Copy. Read again after a sync, a send and a shielding.
- **Contacts.** Name and address, kept on this computer only in
  `<app data dir>/contacts-<test|main>.json` (`{"version": 1,
  "contacts": [{"name", "address"}]}`), written to a temporary file and
  renamed. Names are unique ignoring case; an address must be payable on
  the wallet's network. The send form has a contact picker, and "Pay" on
  a contact fills it in.
- **Account.** The app uses account 0 of the device, as before.

## Where the wallet lives

`<app data dir>/wallets/<test|main>`, a zecz data directory (the same
format as `zecz --data-dir`):

- Linux: `~/.local/share/com.leekwallet.zcash/wallets/test`
- macOS: `~/Library/Application Support/com.leekwallet.zcash/wallets/test`
- Windows: `%APPDATA%\com.leekwallet.zcash\wallets\test`

Testnet by default. "Network and server" (top of the window) switches
between Testnet and Mainnet; mainnet takes a confirmation. The choice
and each network's server are saved in `<app data dir>/settings.json`
(`{"version": 1, "network": "test"|"main", "servers": {"main": "…"}}`;
a network without an entry uses its default, `https://testnet.zec.rocks:443`
or `https://zec.rocks:443`). Switching closes the device session, drops a
pending send and opens the other network's wallet directory and address
book; neither network's files are touched by the other. zecz-core
refuses a server whose chain (GetLightdInfo `chain_name`) is not the
wallet's, before it stores anything.

On mainnet the window carries a red "MAINNET — real funds" bar, and a
send or shielding needs one more step after the review: a tick box
("This sends real ZEC on mainnet…"), without which Sign stays disabled
and `sign_and_send` refuses (`confirmMainnet`). The device is asked with
the wallet's network ("main") in every call; a session paired for one
network is refused for the other.

`LEEKZ_NETWORK=main|test` and `LEEKZ_SERVER=https://host:port` still
override the saved choice for one run, without saving it. The first
mainnet test with the board is `docs/zcash/MAINNET.md`.

## Requirements

- Rust (stable) and Node 22+ with pnpm (`corepack enable`; the version is
  pinned in `package.json`).
- Linux: `libwebkit2gtk-4.1-dev libayatana-appindicator3-dev librsvg2-dev
  libudev-dev libdbus-1-dev libssl-dev patchelf` (Debian/Ubuntu names).
  The serial port needs read/write access (the `dialout` group on most
  distributions); Bluetooth needs BlueZ running.
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
cargo test                                   # offline, includes zdev
cargo test --test backend -- --ignored       # import + sync on testnet
cargo test --test device_zdev -- --ignored   # a device send on testnet
```

`tests/device_zdev.rs` drives the device flows against `zdev`
(`firmware/sim`, built with `make -C firmware/sim build/zdev` on first
use; the tests skip loudly if that fails). zdev runs the firmware's
protocol, session, both signers and review pages on a PTY with the public
test seed and answers its prompts from stdin. The tests pair over "USB",
import the account (birthday from the user, then from the device for a
second companion), verify the address and a foreign one, sign the
`iw_send_shielded_compact` vector and prove and extract the result, and
check a rejection on the device, a device with another seed, and the
error sentences. The ignored test imports zdev's account near the tip,
syncs, and sends to itself if the account holds enough (without
broadcasting unless `LEEKZ_TEST_BROADCAST=1`). Bluetooth needs a radio and
a board; it is only compiled here.

`tests/backend.rs` also checks the wallet screens offline: the history
of an offline wallet holding a real transaction (vector
`iw_send_shielded`, soft-signed, proven and stored with
`Wallet::store_transaction`) and its JSON, rotating addresses and the QR
code, the shield refusals (watch-only, not synced), and the contacts
file (sorting, unique names, foreign-network addresses, rename, remove,
no temporary file left, an unreadable file). The refusals of a shield
the fee would swallow are tested in `host/zecz-core/tests/shield.rs`.

The testnet test imports the public test account (the 24-word
"abandon ... art" mnemonic, UFVK from `vectors/keys.json`) as watch-only
about 2,500 blocks below the tip, syncs it through
`https://testnet.zec.rocks:443`, and checks the progress events,
balances, addresses and the order of the history.

CI (`.github/workflows/desktop.yml`) builds the app on Ubuntu, Windows and
macOS and runs the offline tests and `host/` tests on Ubuntu.
