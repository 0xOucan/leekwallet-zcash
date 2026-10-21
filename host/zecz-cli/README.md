# `zecz`: the LeekWallet Zcash command line

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

## Build

```sh
cd host
cargo build --release -p zecz-cli      # target/release/zecz
target/release/zecz --help
```

Rust as pinned in [docs/TOOLCHAIN.md](../../docs/TOOLCHAIN.md). The board is
reached over its USB serial port (for example `/dev/ttyACM0` on Linux), or
over the PTY of [zdev](../../firmware/sim/README.md), the simulated board.

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

## Commands

The examples use `$T` from above. Amounts are in zatoshis, or in ZEC with
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

`zecz` has no tests of its own; its behaviour is tested in
[`zecz-core`](../zecz-core/README.md) (`cargo test` in `host/`).
