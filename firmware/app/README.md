# LeekWallet Zcash Companion

Shown as **Zcash LeekWallet** (launcher, window title), application id
`io.leekwallet.zcash.companion`. The companion app of the LeekWallet Zcash
board, **Zcash only**: one
[Tauri v2](https://tauri.app) codebase for Linux, macOS, Windows and Android,
talking to the board over USB or Bluetooth on every platform. It pairs with
the board, unlocks it, applies a passphrase, and is a full Zcash wallet
(sync, receive, send and shield signed on the board, history, contacts) for
mainnet and testnet in one app.

It began as LeekWallet's EVM companion. Everything EVM (chains and RPC,
balances and tokens, EVM send and signing, mini apps, WalletConnect, the
`ur:eth-*` QR air gap, blind-signing settings, the EVM browser extension) was
removed in October 2026: the board's firmware signs Zcash only. Git history
keeps the old code; a future app for BTC, Zcash and EVM would be a separate
project.

```
app/
├── packages/core/     @leekwallet/core: framing, CBOR, session, the z* calls
├── src/               UI (shared by every platform)
└── src-tauri/         Rust shell: transports, the Zcash wallet (zecz), capability allowlist
```

## Building the Tauri desktop shell

The web shell runs in any browser against the mock with no extra tooling
(pairing, PIN and passphrase only: the Zcash screens need a board or zdev). The
**Tauri** build additionally needs GTK and WebKit development packages, which
are not installed by default on Ubuntu:

```bash
sudo apt install -y libwebkit2gtk-4.1-dev libsoup-3.0-dev libssl-dev librsvg2-dev
```

One line on purpose. A backslash-continued command loses its continuations when
pasted through some terminals, and the shell then reads the remaining package
names as commands — `libssl-dev: command not found` rather than anything that
suggests a paste problem.

`libxdo-dev` and `libayatana-appindicator3-dev` are only needed for global
shortcuts and a tray icon, neither of which this app uses.

**Rust 1.88 or newer** is also required — Tauri's dependency tree moved past
what Ubuntu's packaged `rustc` provides. `rustup default stable` is enough; a
distro Rust will fail with `rustc 1.85.1 is not supported by the following
packages`.

Without any of this, `pnpm dev` and the mock still cover the pairing, unlock
and passphrase screens, not the native window, the real transports or the
wallet.

### Running the native shell

Normally one command:

```bash
pnpm tauri dev
```

`beforeDevCommand` in `tauri.conf.json` is `pnpm dev`, so this starts Vite and
then the app, in one terminal, and stops both together. Prefer it.

The two-terminal form below exists for one case: iterating on the **Rust** side
without restarting Vite every time, since `cargo run` rebuilds only the binary.
It is also what the failure mode looks like, so it is worth understanding.

```bash
# Development, split: Vite serves the frontend, edits reload live.
pnpm dev                        # terminal 1, must be running first
cd src-tauri && cargo run       # terminal 2
```

```bash
# Standalone: uses the built dist/, no dev server needed.
pnpm build
cd src-tauri && cargo run --release
```

A **debug** build loads `devUrl` from `tauri.conf.json`, which is
`http://localhost:1420`. A **release** build loads `frontendDist`. So
`cargo run` without `pnpm dev` running produces exactly one symptom — a blank
window complaining it cannot connect — and the fix is whichever half is
missing. `pnpm tauri dev` cannot land in that state, because it starts both.

Note that `cargo run` **does not** run `beforeDevCommand`; only the `tauri` CLI
reads that key. Running the Rust binary directly is not a shortcut past the
frontend, it is a way to start half the app.

`libEGL warning: DRI3 error` on startup is harmless; the window falls back to
software rendering.

## Setup

Requires **Node 22+** and **pnpm 9+**. Get pnpm through corepack, which ships with Node — no
global install, and the version is pinned by `packageManager` in `package.json`:

```bash
corepack enable pnpm      # once per machine
cd app
pnpm install
```

| Command | From | Does |
|---|---|---|
| `pnpm install` | `app/` | Install the workspace |
| `pnpm test` | `app/` | Run every package's tests |
| `pnpm typecheck` | `app/` | Strict `tsc` over all sources |
| `pnpm --filter @leekwallet/core test` | anywhere | One package only |

Tests run under Node's native type stripping, so there is no build step and no bundler in the
loop. Two consequences worth knowing: `enum` is unavailable (it needs code generation — use a
const object), and `tsc` is typecheck-only, never emitting.

`tsconfig.json` enables `strict`, `noUncheckedIndexedAccess` and `exactOptionalPropertyTypes`.
That is deliberate for code that parses bytes off a wire on behalf of a signing device — the
index checks alone caught four real unguarded accesses in the framing layer.

## Dependencies and supply chain

The app itself ships **five** npm packages: `@noble/ciphers`, `@noble/curves`
and `@noble/hashes` (the session), `qrcode-generator` (receive QR codes) and
`zxing-wasm` (reading a payment QR code in Send, see "QR scanner").
Everything else in the lockfile is build tooling: TypeScript, Vite and its
platform binaries, the Tauri CLI and Node types. That number is the strongest
control available here, and it is worth defending — every addition is a party
who can push code into a process that talks to a signing device.

`.npmrc` sets three things:

| Setting | Why |
|---|---|
| `ignore-scripts=true` | Most npm supply-chain incidents execute through a `postinstall`. pnpm 9 still runs them by default; pnpm 10 changed that, and we are on 9.x. Nothing here needs them, and the build was verified without. |
| `save-exact=true` | An install today and an install next month resolve identically, rather than relying on a range being honoured. |
| `prefer-frozen-lockfile=true` | A dependency change should be a reviewed commit, not a side effect of running install. |
| `minimumReleaseAge=10080` | Seven days. Refuses anything published in the last week — see below. |

### The seven-day gate

`minimumReleaseAge` is the strongest single answer to the attack pnpm does not
otherwise prevent. A compromised package is usually spotted and yanked within
hours; refusing anything published in the last week lets the ecosystem do the
detection, and this project never installs the bad version at all. The cost is
that a genuine fix is a week late, which for a wallet is the right trade.

**It requires pnpm 10.16 or newer.** This project is pinned to 9.15.3, where the
key is silently ignored, so the line is written and inert. To activate it:

```bash
pnpm add -g pnpm@latest      # or: corepack use pnpm@latest
```

then bump `packageManager` in `package.json`. Note that pnpm 10 also blocks
install scripts by default, which `ignore-scripts` already does here.

> Watch out for one trap while doing this. `corepack prepare` may fail with
> `Cannot find matching keyid` on older Node, because its bundled npm signing
> keys have rotated. The advertised workaround is `COREPACK_INTEGRITY_KEYS=0`,
> which disables signature verification — do not use it. Turning off signature
> checking to install a supply-chain mitigation defeats the mitigation.

### What pnpm does and does not do

It gives a strict `node_modules` layout, so nothing can use a package it did
not declare, and a lockfile with integrity hashes, so a resolved package cannot
be swapped underneath you.

**It does not stop a legitimate package from publishing a malicious version.**
That arrives through the front door on your next install or update, and no
package manager prevents it. Neither does it help with typosquatting, or with a
transitive dependency changing hands.

### Why that is survivable here

The architecture, not the package manager, is what contains this. The seed never
leaves the device; the board parses every PCZT itself, recomputes the sighash,
and shows every output, the memo and the fee on its own screen before a
physical button press. A compromised npm package can show you a false preview,
which is why the app's review is advisory, but it cannot extract a key or sign
anything you did not approve on the board.

## Layering

The rule that keeps a Tauri-to-something-else migration cheap, and keeps the
Android and desktop builds honest, is that **no device logic lives in the
shell**. `packages/core` has no platform dependencies at all — it is pure
codec and state machine, testable under plain Node:

```bash
cd app/packages/core && npm test
```

The Rust side moves bytes (USB and BLE, the same commands on desktop and
Android, see ANDROID.md) and, with the `zcash-wallet` feature, runs the
wallet engine; the protocol above the transports is identical on both.

## Running it

```bash
corepack enable pnpm
cd app
pnpm install
pnpm dev          # http://localhost:1420
```

The shell runs in a plain browser against the mock device, so the pairing,
unlock and passphrase screens can be exercised with no hardware and no Rust
toolchain; the Zcash screens need the native app and a board (or zdev).
`pnpm build` produces the static bundle Tauri wraps.

The mock is configured with 250 ms of latency on purpose. A mock that answers
instantly hides every place the UI forgot to show that it is waiting, and a
hardware wallet spends real seconds deriving keys.

### Theme

Light, dark, and system, cycled from the status bar and remembered. System is a
distinct state rather than an initial guess — a user who toggles once must be
able to get back to following their OS.

## Zcash

After unlocking, the Zcash wallet is the home screen, for the LeekWallet
Zcash firmware (docs/zcash/ROADMAP.md 6.14), and the Device panel shrinks to
the link and Disconnect. It runs over the encrypted session: the `z*` methods
of firmware/docs/PROTOCOL.md are `packages/core/src/zcash.ts`, called through
main.ts's Client, so the board is paired once per connection and requests
never interleave. A locked or disconnected board hides the whole wallet
screen and the account read from the board (`setShellVisible` in main.ts).

**Network.** Testnet unless switched; Mainnet asks first ("Mainnet moves real
ZEC") and keeps a danger banner up while it is on. Each network has its own
wallet, server and address book; nothing is shared between them. A board
that refuses a request for the other network (its network guard, 0x0528)
gets a notice on every tab saying which network each side is on, with the
board's own words, and one button, *Switch the app to Mainnet/Testnet*,
which switches and reads the account again (`src/network-guard.ts`; no
Mainnet question there: the board's setting is the user's choice already
made, and the Mainnet banner stays up). Firmware that reports its network
in `getStatus` (`network`) is followed on connect and when it changes on
the board, before anything is refused; older firmware works as before.
Every other board error is displayed verbatim, with its code.

**Automatic, as in Zodl.** Connecting is one guided flow with no other
click: Connect (or, from the launcher, *Reconnect your board*) pairs (compare
the passkey, ALLOW on the board), asks the board for its PIN, reads the
account (`zGetAccount`, once more after 1.5 s on 0x0300), and opens the
wallet of that network, seed fingerprint and account, creating it when this
device has none (`src/zcash-auto.ts`): born at the board's birthday for that
network, or, when the board has none, at the chain tip minus 100 blocks,
which is then written to the board with `zSetAccountMeta` as the desktop
app does. Then it syncs (`src/sync-scheduler.ts`): on open, every 75 s while
the app is in front, right after a send or a shield; never two at once, never
between a proposal and its finish, and not in the background (Android, a
minimised window). The home screen says *Synced at block N*, *Syncing X%* or
the error, with *Retry*. The manual buttons stay for troubleshooting: Board →
*Read the board's account*, and the wallet's *set up by hand* form, shown
when the automatic set-up fails.

**The remembered board** (`src/board-memory.ts`): the last board's link and,
over Bluetooth, the peer id and advertised name, kept in the webview's
storage; never a key, passkey or anything of the session, which is a fresh
handshake (passkey, ALLOW) on every connection. *Forget it* in the Device
panel drops it. Reconnecting over Bluetooth goes to that id
(`ble_connect`), so another board in range is never picked: a 4 s scan,
then the id directly; a board found neither way is reported as "The board
is not advertising: it may still be connected to this or another app.
Restart the board or wait ~30 s", plus that a button press on the board
makes it advertise again (it stops two minutes after a disconnect).

**The link is closed when nobody uses it** (`src-tauri/src/link.rs`,
`src/link-events.ts`): the board serves one peer and does not advertise
while connected, so a link left open keeps every later connection out.
The app sends a GATT disconnect (and releases USB) after 30 s in the
background (Android onPause; not at once, since Android pauses the app
for its own permission dialogs), when it closes, and at start for a link
an earlier run of the same process left open (Android often keeps the
process when the activity is closed with Back). A request waiting on the
board is never cut. The session on screen then ends with the reason, and
*Reconnect your board* starts again.

**Board** (any build):
- Read the board's account for the network and account number: unified and
  transparent address (QR, copy), viewing key, seed fingerprint and birthday.
- *Verify on the board*: `zShowAddress`; the board pages the address and the
  user answers there.
- *Set on the board*: the birthday for that network (`zSetAccountMeta`).

**Sign a file**: open an unsigned `.pczt` made by zecz, the desktop app or
Zodl; the board checks it, shows every output and the fee, and signs on ACCEPT.
The `.zsig` is saved to the downloads folder (the app's own `Download` folder
on Android, under `Android/data/io.leekwallet.zcash.companion/files/`). The file is
sent as it is: its `.zsig` names the hash of exactly those bytes, which is what
the wallet that kept the PCZT checks. A compact view is the builder's choice
(`zecz send --compact`); the wallet below always builds one.

**Wallet** (builds with the `zcash-wallet` feature, the default): the board's
viewing key synced from lightwalletd by zecz (host/zecz-core), the engine of the
CLI and the desktop app. Its import, sync, propose, finish, address book and
server settings are the desktop app's own modules, compiled in from
`desktop/src-tauri/src` (`src-tauri/src/lib.rs`); `src-tauri/src/zwallet.rs` is
the commands. The screens follow Zodl's as a feature reference (nothing of its
code is used):

- *Wallet* (home): the total (hideable), shielded (Orchard + Ironwood, each
  shown when both hold funds) and transparent, what is spendable now and what
  is waiting for confirmations, the sync state, a *Shield* button whenever
  transparent funds exist (enabled once they are spendable), and recent
  activity.
- *Receive*: a fresh diversified unified address each time Receive opens or
  on *New address* (every one reaches the wallet; while a sync holds the
  wallet, the last one stays and a new one follows the sync), and a toggle to
  *Board address (index 0)*: the same address the board shows on its own
  Receive screen, to compare. *Verify on the board* sends the address shown to
  `zShowAddress`; the board derives its receiver at that diversifier index and
  refuses one that is not its own. Transparent: the account's default address
  (index 0), or *New transparent address*: the transparent receiver of the
  next diversified address, checked on the board through the unified address
  that carries it (the board compares its key at that index). Both say it is
  public and to shield what arrives. QR and copy (`src/receive-model.ts`).
- *Send*: *Scan QR* (a Zcash address or a ZIP 321 `zcash:` request; pasting
  a `zcash:` link into To does the same), *Pay a contact*, recipient, amount,
  memo with its byte count (none to a transparent address), a review with fee
  and change, and on mainnet a confirmation to tick. Then: the board's seed is
  checked against the wallet's, the compact signer view is signed on the
  board, and the wallet proves, broadcasts and shows the txid.
- *Activity*: history with memos, recipients, fee, and an explorer link.
  Each row is named by zecz's kind (`src/tx-labels.ts`): Received, Sent,
  *Sent to yourself* (shown with what it cost, the fee), Shielded, Moved
  between pools; a kind added later reads as its own name, never
  "undefined".
- *Contacts*, *Settings* (server per network, checked before it is saved;
  wallet info; forget this network's wallet).

**QR scanner.** Send only, Zcash only; no QR flow for pairing or signing.
`packages/core/src/zip321.ts` parses the code strictly: the address (unified,
Sapling, transparent, TEX), `amount` (at most 8 decimals, at most 21 million),
`memo` (base64url, UTF-8 text, at most 512 bytes, never to a transparent
address), `label` and `message`; a repeated or malformed parameter or an
unknown `req-` one rejects the request. Nothing is filled in when the code is
for the other network (said in words: switch the network first) or carries
more than one payment (this wallet pays one recipient at a time). The camera
is read in the webview by the bundled `zxing-wasm` reader (`src/qr-scan.ts`):
`BarcodeDetector` is missing from WebKitGTK and crashed the Android webview
without Play Services, jsQR could not read dense codes off a screen, and
Tauri's barcode-scanner plugin is mobile only. It costs about 1 MB of
WebAssembly and `'wasm-unsafe-eval'` in the CSP. For dense codes on bright
screens (a unified address on the board's 240×240 display): 1920×1080
asked of the camera, continuous autofocus where offered, the middle of the
picture decoded at full resolution every frame, and in rotation the whole
frame, a software contrast pass and the other binarizer
(`src/qr-tuning.ts`). Under the picture: *Brightness* (the camera's
exposure compensation, or a software darkening where it has none),
*Zoom* when the camera has it, and *Switch camera*; the camera chosen is
remembered (`leek.qr.camera`). On Linux the backend enables
WebKitGTK's media stream and grants camera requests only; on Android the
manifest declares the camera (ANDROID.md).

**Which wallet: the seed fingerprint.** When the home screen opens, the
Companion reads the board's account (`zGetAccount`, retried once on 0x0300
while the board's vault is still opening). Its ZIP 32 seed fingerprint and
account, with the network, name the wallet on this device. A BIP39
passphrase on the board (on the board itself, or "Passphrase (hidden
wallet)" here) gives another seed, so another fingerprint, so another wallet,
with its own directory and address book; it never mixes with the wallet
without a passphrase. Applying or clearing a passphrase re-reads the account
and switches wallet. The passphrase panel says this, and that the hidden
wallet's synced data stays on this device under its fingerprint (forget it
in Settings).

Data lives in the app data directory under `zcash/` (on Linux
`~/.local/share/io.leekwallet.zcash.companion/zcash/`, on macOS
`~/Library/Application Support/io.leekwallet.zcash.companion/zcash/`, on
Windows `%APPDATA%\io.leekwallet.zcash.companion\zcash\`, on Android the
app's private storage):
`wallets/<test|main>-<zfp16>-<account>` (zfp16: the first 8 bytes of the
seed fingerprint, in hex), `contacts-<test|main>-<zfp16>.json`, and
`settings.json` (the server per network, shared): viewing keys and what they
have seen, never a spending key. A wallet from the first builds
(`wallets/<net>`, `contacts-<net>.json`) is moved to its keyed place the
first time the board account it belongs to is read; another seed's leaves it
alone.

Tests: `pnpm test` includes `packages/core/test/zcash.test.ts` (the golden
`.zsig`, reply checks, chunking, amounts), `packages/core/test/zip321.test.ts`
(payment requests, address networks) and `test/companion-flow.test.ts` (the
sync schedule, the remembered board, Receive's rotation and board view, the
automatic set-up over a scripted board, the activity labels, the
wrong-network notice and switch, the link's close-on-start, and the
scanner's camera memory, sliders, crops, passes and contrast); `pnpm test:zdev` pairs with zdev
(`make -C firmware/sim build/zdev`, the firmware's protocol on a PTY) using the
Companion's framing and session, and signs the PCZT vectors on both networks, runs the automatic set-up against
zdev on Testnet and Mainnet boards (`--network`) and with a passphrase
(and the one-tap switch after the board's 0x0528), and
checks a rotated address and its transparent receiver on the board;
`cargo test --lib` in src-tauri covers the save command, the import checks
and the background-release timing (`link.rs`),
and `cargo test --lib -- --ignored online` imports and syncs the public test
account against testnet lightwalletd. To drive the desktop app against zdev,
start zdev with `--link /tmp/zdev` and the app with `LEEK_SERIAL_PORT=/tmp/zdev`.

## The new application id (October 2026)

The identifier was `com.leekwallet.companion` ("LeekWallet") and is now
`io.leekwallet.zcash.companion` ("Zcash LeekWallet"), so that a future EVM
LeekWallet app can sit beside it. The app data directory follows the
identifier, so nothing carries over by itself:

- **Android**: the new APK installs beside the old app, not over it.
  Uninstall `com.leekwallet.companion` when you like; in the new app, read
  the board's account and set the wallet up again: it syncs again from its
  birthday.
- **Desktop**: the old data is in `~/.local/share/com.leekwallet.companion/`
  (Linux), `~/Library/Application Support/com.leekwallet.companion/`
  (macOS) or `%APPDATA%\com.leekwallet.companion\` (Windows). Delete it, or
  move its `zcash/` folder into the new directory before the first start to
  keep the synced wallets, server and contacts (viewing keys only).

## Diagnostics

*Diagnostics → Copy diagnostics* puts a plain-text report on the clipboard
for a bug report:

- **Connection**: state, backend badge, selected link, the transports this
  build has, whether the session is encrypted, and the device status
  (locked or not, wallet or temporary seed, passphrase on or off).
- **Board**: firmware, model and chain from `getFeatures` (read once per
  connection); from `zGetFeatures` over the paired session, asked when the
  report is made: the signer backend, branch IDs, PCZT and chunk limits and
  address display; the board's network setting when its `getStatus`
  reports it (older firmware does not: it refuses requests for the other
  network, and the report says so); the board account read (account,
  network, seed fingerprint, birthday).
- **Zcash**: the Companion's network and account; the wallet's lightwalletd
  server (and the default), when it last answered, the wallet's account and
  ZFP, birthday, sync height against the tip, and how the last sync ended.
- **Device log**, newest first.

Never in it: the passkey, the passphrase, viewing keys, addresses, the
wallet's data directory (it carries the user name on a desktop), or anything
typed into a form.

## Status

Implemented and tested: framing (length-prefixed frames, BLE chunking,
hostile lengths rejected before allocating), the CBOR subset (RFC 8949
appendix A), the session (X25519 commit-then-reveal, passkey, ChaCha20-
Poly1305), the mock device (session, PIN, wallets, passphrase; its replies
compared with the firmware's own by `mock-conformance.test.ts`, regenerated
by `make -C firmware/sim conformance`), the z* calls against zdev, USB and
BLE on desktop and Android, and the Zcash wallet.

Tests: `pnpm typecheck`; `pnpm test` (packages/core and the flasher);
`pnpm test:zdev` (against zdev); `cargo test --lib` in src-tauri.

See [../docs/PROTOCOL.md](../docs/PROTOCOL.md) for the wire format and
[../docs/DESIGN.md](../docs/DESIGN.md) for the visual language.
