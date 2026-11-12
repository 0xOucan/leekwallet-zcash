# `zecz`: the LeekWallet Zcash command line

Website: [zcash.leekwallet.xyz](https://zcash.leekwallet.xyz) · Demo: [2-minute video](https://www.youtube.com/watch?v=8F6_8AhsnAA) · X: [@gmLeekWallet](https://x.com/gmLeekWallet) · [@0xOucan](https://x.com/0xOucan)

`zecz` is our command-line wallet for the LeekWallet Zcash board. It holds
the board's viewing key only (a watch-only account), syncs from a
lightwalletd server, builds each transaction as a PCZT (ZIP 374), lets the
board check and sign it, then proves it, applies the signatures and
broadcasts it. It also writes and reads `.zkv` seed cards for the board's
SD storage mode.

It is **not an official Zcash tool**. It is ours, built on our engine
[`zecz-core`](../zecz-core/README.md), which uses librustzcash (the
official Zcash Rust libraries). The official tools are full nodes and their
wallets: zcashd (retired), zebrad (a full node, no wallet) and Zallet (the
full-node wallet that replaces zcashd's, in beta). None of them knows the
board. `zecz` is a light wallet and needs no node of its own.

All the wallet logic lives in `zecz-core`; this crate parses arguments and
prints.

The board reaches `zecz` four ways: **USB**, **Bluetooth LE**, **QR** and
the **SD card**. Over USB and Bluetooth every connection pairs with the
six-digit code (no remembered hosts) and runs the same encrypted session.

### The LeekWallet SDK in `zecz`

`zecz` uses the LeekWallet SDK's Rust kit,
[`leek-zcash`](../leek-zcash/README.md), and the crates it is built on:

| Part | Crate | Used for |
|---|---|---|
| SDK: features | `leek-zcash` (`get_features`, `Features`, `BOARD_UPDATE_TEXT`) | what the board's firmware supports (`device-features`) and the "Update your board" notice after pairing |
| SDK: checks | `leek-zcash` (`preflight`, `check_broadcast`), in `zecz-core`'s tests | the NU7 checks on what `zecz` builds (`tests/nu7.rs`) |
| Board client | `leek-device`, which `leek-zcash` drives and re-exports | pairing with the six-digit code, the encrypted session, `zGetAccount`, `zShowAddress`, `zSign`, the `.zsig` format |
| QR | `leek-ur`, the encoder the SDK's `QrEncoder`/`QrScanner` wrap | `ur:leek-pczt` out, `ur:leek-zsig` back |
| Links | `leek-transport-serial` (USB), `leek-transport-ble` (Bluetooth, the Zcash LeekWallet app's btleplug transport) | the frames to the board |

The wallet itself (sync, PCZT building, proving, broadcast) is `zecz-core`
on librustzcash, not the SDK.

## Build

```sh
cd host
cargo build --release -p zecz-cli      # target/release/zecz
target/release/zecz --help
```

Rust as pinned in [docs/TOOLCHAIN.md](../../docs/TOOLCHAIN.md). The board is
reached over its USB serial port (for example `/dev/ttyACM0` on Linux), over
Bluetooth LE, or over the PTY of [zdev](../../firmware/sim/README.md), the
simulated board.

Bluetooth is in the default build (feature `ble`, btleplug). On Linux it
talks to BlueZ over D-Bus and needs `libdbus-1-dev` (Debian, Ubuntu) or
`dbus-devel` (Fedora) to build; a computer without an adapter builds and
runs it, and only `--ble` reports the missing adapter. For a zecz without
Bluetooth:

```sh
cargo build --release -p zecz-cli --no-default-features   # USB, QR and SD only
```

## Networks and data directories

Global options, given before or after the command:

| Option | Default | What |
|---|---|---|
| `--network test\|main` | `test` | The Zcash network |
| `--data-dir DIR` | `./zecz-data` | Where the wallet lives |
| `--server URL` | `https://testnet.zec.rocks:443` (test), `https://zec.rocks:443` (main) | The lightwalletd gRPC endpoint |

A data directory belongs to one network. It holds:

```
DIR/network          "test" or "main", written when the wallet is created
DIR/wallet.sqlite    the wallet database (librustzcash's zcash_client_sqlite)
DIR/pending/         full PCZTs waiting for their signatures, by SHA-256 of the signer view
DIR/sent/            each finished transaction, as <txid>.tx
```

Opening a directory with the other network is refused, so keep one
directory per network, for example:

```sh
T="target/release/zecz --network test --data-dir ~/zecz-test"
M="target/release/zecz --network main --data-dir ~/zecz-main"
```

Every command that talks to the server first checks the chain it follows
(GetLightdInfo). A mainnet wallet refuses a testnet server, and the other
way round.

## Links to the board

Every command that talks to the board (`device-features`, `device-init`,
`device-show-address`, `sign-device`) takes the same link options:

| Option | What |
|---|---|
| `--port PATH` | USB: the board's serial port (`/dev/ttyACM0`), or a zdev PTY |
| `--ble` | Bluetooth LE (the same as `--link ble`) |
| `--ble-addr ADDR` | Bluetooth: this board, by the address `device-scan` lists; implies `--ble`. Needed only when more than one board is advertising |
| `--ble-scan SECS` | Bluetooth: how long to scan (default 4, 1 to 60) |
| `--link usb\|ble` | The link by name; the default is `ble` when `--ble` or `--ble-addr` is given, else `usb` |

`--port` with `--ble`, or `--link usb` with `--ble-addr`, is refused.

### Bluetooth

The board advertises on one link at a time: on the board, **Settings →
Link → Bluetooth**. Then:

```sh
$T device-scan                       # list boards: address and name
$T device-features --ble             # pair with the code, show the firmware
$T device-init --ble                 # pair, "Share view key?" on the board, import
$T sign-device --ble --in tx.pczt --out tx.zsig
$T device-show-address --ble --ble-addr 28:84:85:9F:D2:0E
```

`zecz` scans for the LeekWallet service UUID (`6c65656b-7761-6c6c-6574-000000000001`;
never by name), connects to the only board found or to `--ble-addr`, and
then does exactly what it does over USB: a fresh pairing with the six-digit
code on both screens and ALLOW on the board, the same encrypted session, and
the board's own prompts (PIN, "Share view key?", the review pages). Nothing
about the board is remembered between runs. When the command ends `zecz`
unsubscribes and disconnects, so the board is free for the next connection.

When it cannot connect it says why: no adapter (`bluetoothctl show`),
Bluetooth switched off, a Bluetooth permission (BlueZ over D-Bus on Linux;
System Settings → Privacy & Security → Bluetooth on macOS), no board
advertising ("On the board: Settings → Link → Bluetooth"; a button press
makes it advertise again), or more than one board and no `--ble-addr`.

Bluetooth in `zecz` is new and not yet tested on the board; the same
transport is the one the Zcash LeekWallet app uses on the desktop.

## Commands

The examples use `$T` from above; `--port /dev/ttyACM0` can be `--ble`
everywhere. Amounts are in zatoshis, or in ZEC with
`--zec`.

### Set up

**`device-init`**: pair with the board, read its account and import it as
watch-only.

```sh
$T device-init --port /dev/ttyACM0
$T device-init --port /dev/ttyACM0 --account 0 --birthday 3500000 --name leekwallet
```

`zecz` prints a six-digit passkey; check that the board shows the same and
press ALLOW there. If the board is locked it asks for the PIN on the board
(the PIN never crosses the wire). The account's birthday comes from the
board when it knows one; otherwise pass `--birthday`, and `zecz` records it
on the board for the next companion (the board keeps one per network).
`zecz` checks that the board's own address matches its viewing key before
importing. Prints the address and the ZFP (short seed fingerprint) to
compare with the board's screen.

**`device-scan`**: list the boards advertising over Bluetooth (address and
name), for `--ble-addr`. It does not connect.

**`device-features`**: pair with the board (the six-digit code) and show its
firmware, the network it is set to and what it can sign. A check of the
link; it shares no key.

```sh
$T device-features --ble
```

**`init`**: import an account from a viewing key, without the board.

```sh
$T init --ufvk uviewtest1... --birthday 3500000 \
        --seed-fingerprint <64 hex chars> --account-index 0
```

**`tip`**: show the server's chain, tip height and version (useful to pick a
birthday). It warns, rather than refuses, when the server follows the other
network.

### Use the wallet

**`sync`**: scan the chain until the wallet is up to date.

```sh
$T sync                 # --batch 1000 blocks per scan batch
```

After the scan it fetches the full transactions librustzcash asks for and
compares the wallet's transparent coins with the server's UTXO set, so a
coin spent elsewhere (a shield made by another wallet with the same keys)
leaves the balance.

**`balance`**: balances per pool (Orchard, Ironwood, transparent), total and
spendable.

**`address`**: the default unified address, its transparent address and the
Orchard-only unified address.

**`history`**: the account's transactions, newest first, with recipients and
memos. A send to your own address reads "Sent to yourself", with its fee as
the amount.

```sh
$T history --limit 20
```

**`device-show-address`**: show this wallet's address on the board; you
confirm there that it is the same.

```sh
$T device-show-address --port /dev/ttyACM0
```

### Send and shield

A send takes three steps: build, sign on the board, finish.

**`send`**: build an unsigned PCZT for the board.

```sh
$T send --to <address> --amount 0.001 --zec --memo "hi" --compact --out tx.pczt
```

The recipient may be a unified, Orchard-only unified or transparent address;
a memo only goes to shielded recipients. `--compact` writes the compact
signer view (v2 PCZT, about 1.4 KB instead of about 2.8 KB): the board
rebuilds the fields it leaves out, and QR takes half the frames. The full
PCZT stays in `pending/` for proving. `zecz` prints the payments, change,
fee and the SHA-256 of the file.

**`shield`**: build a PCZT that moves every transparent coin of the account
into the shielded pool (Ironwood from NU6.3, Orchard before). Coins below the
ZIP 317 marginal fee are left as dust, and a balance that would all go to
the fee is refused.

```sh
$T shield --compact --out s.pczt
```

**`sign-device`**: upload the PCZT, review it on the board, write the
`.zsig`.

```sh
$T sign-device --port /dev/ttyACM0 --in tx.pczt --out tx.zsig
```

Read every page on the board (outputs, memo, fee, network) and press ACCEPT
on the last one. The `.zsig` holds the signatures only, bound to the SHA-256
of `tx.pczt` ([FORMATS.md](../../docs/zcash/FORMATS.md)).

**`finish`**: check the `.zsig` against the file that was signed, apply the
signatures to the pending full PCZT (each verified against its key), prove,
extract, store and broadcast.

```sh
$T finish --pczt tx.pczt --sigs tx.zsig
$T finish --pczt tx.pczt --sigs tx.zsig --no-broadcast   # store only; raw tx in sent/
```

`--pczt` is the same file given to the board, from the same data directory.

### The SD card instead of a cable

For the airgap, the card carries the files (layout in
[FORMATS.md](../../docs/zcash/FORMATS.md)):

```sh
$T send --to <address> --amount 0.005 --zec --compact --out /media/$USER/SD/LEEKZ/unsigned.pczt
# on the board: SD Card > Sign transaction; it writes LEEKZ/signed.zsig
$T finish --pczt /media/$USER/SD/LEEKZ/unsigned.pczt --sigs /media/$USER/SD/LEEKZ/signed.zsig
```

### QR instead of a cable

Opt-in, for a board kept off USB and Bluetooth. It is
slower than a cable (a 1.4 KB compact view is 28 frames at the default 50-byte fragments
and 500 ms a frame; the colour kit read it in about 70 s at 2x zoom); USB and Bluetooth stay the everyday
links. Never for pairing. The UR (`ur:leek-pczt` out, `ur:leek-zsig` back)
is [`leek-ur`](../leek-ur), the one the Zcash LeekWallet app uses.

```sh
# 1. Build the send and show it as an animated QR in this terminal
#    (--qr implies --compact). Enter or Ctrl-C stops the loop.
$T send --to <address> --amount 0.001 --zec --out tx.pczt --qr
#    A terminal too small for the code: an offline HTML page instead,
#    opened in a browser, full screen.
$T send --to <address> --amount 0.001 --zec --out tx.pczt --qr-html tx-qr.html
#    Show an existing PCZT again, denser or slower:
$T qr-show --in tx.pczt --qr-fragment 40 --qr-interval 800

# 2. On the board: Scan, point its camera at the code, review, ACCEPT.
#    The board then shows its signature as a QR code.

# 3. Read it: from the webcam (a build with --features qr-camera, Linux
#    V4L2), or from a file of UR parts, one per line, from any scanner.
$T qr-scan --pczt tx.pczt --out tx.zsig
$T qr-scan --pczt tx.pczt --out tx.zsig --from-file parts.txt

# 4. As with every other link.
$T finish --pczt tx.pczt --sigs tx.zsig
```

`--qr-fragment` (default 50 bytes; 40 was the fastest on the board bench, 90 and up rarely decoded): smaller is fewer modules per code,
easier for the board's camera; larger is fewer frames. `--qr-interval`
(default 500 ms). Every frame of a loop is the same QR version (error
correction L, alphanumeric), drawn black on white with a four-module quiet
zone. `qr-scan --pczt` checks the `.zsig` names that PCZT's hash before it
writes it; `finish` checks it again and verifies every signature. The
camera reader wants a YUYV webcam (`--camera /dev/video0` by default) and
gives up after `--timeout` seconds (180).

For the camera bench, `qr-show --sweep` steps through presets (90/500,
90/350, 120/400, 150/350, 150/250, 200/300; or `--presets 90/500,150/250`),
each labelled on screen. Start a scan on the board, press Enter when the
board says it is complete, and the time from the preset's first frame is
printed; at the end, a table for [QR-BENCH.md](../../docs/zcash/QR-BENCH.md):

```sh
$T qr-show --in tx.pczt --sweep
```

### `.zkv` seed cards

A `.zkv` file is a seed sealed under a passphrase, for the board's SD storage
mode (format in [FORMATS.md](../../docs/zcash/FORMATS.md)). The board can
write one itself (SD Card > Save open seed).

**`zkv-new`**: seal a seed phrase into a `.zkv` file.

```sh
ZECZ_ZKV_PASSPHRASE='...' $T zkv-new --mnemonic "word1 ... word24" \
    --out /media/$USER/SD/SEED1.ZKV --birthday 3500000 --label main
```

Options: `--account` (default 0), `--label`, `--iterations` (default
20,000). The file passphrase comes from `ZECZ_ZKV_PASSPHRASE`, or one line
of standard input. The board lists 8.3 names such as `SEED1.ZKV`.

**`zkv-open`**: open a `.zkv` and show what the board will load: words,
label, account, birthday, ZFP and viewing key. It never prints the phrase.

```sh
$T zkv-open --in SEED1.ZKV                  # --passphrase P: a BIP 39 passphrase on top
```

### Development only

These handle a seed phrase on the computer. They exist for tests on testnet
with public test phrases; never use them with real funds.

| Command | What |
|---|---|
| `init-dev --mnemonic "..." --birthday N` | Derive the account from a phrase and import it |
| `sign-soft --mnemonic "..." --in tx.pczt --out tx.zsig` | Sign with keys from a phrase, in place of the board. It does not check what it signs |

## Environment

| Variable | What |
|---|---|
| `ZECZ_ZKV_PASSPHRASE` | The `.zkv` file passphrase for `zkv-new` and `zkv-open` |
| `ZECZ_DEBUG` | Print each request to the board and each reply frame |

## Safety notes

- The board is the only place a spend is approved. Read the pages on its
  screen, not the output of `zecz`.
- Compare the six-digit passkey on both screens before pressing ALLOW.
- `zkv-new`, `init-dev` and `sign-soft` put a seed phrase on the computer.
  For real funds, write `.zkv` cards on an offline computer, or let the
  board write them.
- Testnet is the default. Use a separate `--data-dir` for mainnet, and set
  the board's network (Settings > Network) to match: the board refuses a
  request for the other network (device error 0x0528).
- The project is experimental and not audited; use small amounts on
  mainnet. Checklist: [MAINNET.md](../../docs/zcash/MAINNET.md).

## Tests

`zecz`'s own tests cover its argument parsing and link selection (`cargo
test -p zecz-cli`); the board pick and the Bluetooth error hints are tested
in `leek-device` (`cargo test -p leek-device --features ble`), with no
adapter. The wallet's behaviour is tested in
[`zecz-core`](../zecz-core/README.md) (`cargo test` in `host/`), the
device path against zdev over a PTY.
