# `zecz-core`: the wallet engine

Website: [zcash.leekwallet.xyz](https://zcash.leekwallet.xyz) · Demo: [2-minute video](https://www.youtube.com/watch?v=8F6_8AhsnAA) · X: [@gmLeekWallet](https://x.com/gmLeekWallet) · [@0xOucan](https://x.com/0xOucan)

Our Zcash wallet engine for the LeekWallet board (Rust crate `zecz-core`,
library name `zecz`). It runs on the companion side: it syncs, builds
PCZTs (ZIP 374) for the board, applies the board's `.zsig`, proves and
broadcasts. The board only checks and signs.

Used by the [`zecz` CLI](../zecz-cli/README.md) and the
[Zcash LeekWallet app](../../app/README.md) (desktop and Android, through
`app/zcash-wallet`). The
[Chrome engine](../../chrome/engine) cannot link it (tonic, files, tokio)
and follows the same flow over gRPC-web.

It is built on librustzcash, the official Zcash Rust libraries
(`zcash_client_backend`, `zcash_client_sqlite`, `zcash_keys`, `pczt`,
`orchard` and the rest), pinned to one git commit, the same one as the
firmware's Rust signer and the vector generators: Ironwood (NU6.3) is newer
than any release. Nothing in the crate prints; long operations report
progress through callbacks.

## Accounts

The wallet holds **watch-only** accounts: a unified full viewing key with
Orchard and P2PKH (transparent) parts, no Sapling. The spending keys stay on
the board.

| Item | What |
|---|---|
| `AccountSpec::device(ufvk, seed_fingerprint, account_index, name)` | A board account: the key origin (ZIP 32 seed fingerprint and account index) tells the board which key signs |
| `AccountSpec::view_only(ufvk, name)` | A viewing key with no signer |
| `AccountSpec::from_dev_account(..)` | DEV ONLY, from a phrase (`keys::DevAccount`) |
| `parse_ufvk`, `addresses_for_ufvk` | Decode a UFVK for a network and check its shape; derive its default addresses, e.g. to check what the board reports against its own key |

## Networks and profiles

A `Profile { network, data_dir, server }` says where a wallet lives and which
chain it follows. `Profile::new(network, data_dir, None)` uses the default
server: `https://testnet.zec.rocks:443` on testnet, `https://zec.rocks:443`
on mainnet. The network is `zcash_protocol`'s `Network` (`TestNetwork` or
`MainNetwork`); ZIP 32 coin type 1 or 133.

A data directory is bound to one network by a `network` file beside
`wallet.sqlite`; opening it with the other network is refused. Full PCZTs
wait in `pending/`, finished transactions go to `sent/<txid>.tx`.

**Server chain check.** Every connection asks the server for its chain
(GetLightdInfo) and refuses one that follows the other network
(`check_chain`): a wallet synced from the wrong chain would store its blocks
as its own, and a transaction broadcast there is lost. `tip()` only reports
it. `broadcast_on(network, ..)` checks first; plain `broadcast` does not.

## API

All on `Wallet` unless a free function.

| Call | What |
|---|---|
| `Wallet::init(profile, spec, birthday, on_event)` | Create the wallet and import the account, fetching the tree state before the birthday from the server. Importing the same key again is a no-op |
| `Wallet::init_with_birthday(..)` | The same with an `AccountBirthday` in hand (no network) |
| `Wallet::open`, `Wallet::create` | Open an existing wallet, or create the directory and database |
| `sync(batch, on_event)` | Scan to the tip (`SyncEvent::Started`, `Scanned`, `Transactions`), then `fetch_transaction_data` |
| `fetch_transaction_data(chain)` | The second half of a sync (below) |
| `sync_status`, `balances` | Tip, scan progress; per-pool balances (Orchard, Ironwood, transparent) with spendable and pending |
| `addresses`, `next_address`, `next_shielded_address` | The default addresses; a fresh diversified unified address (with a transparent receiver while the gap limit allows, Orchard-only after); a fresh Orchard-only one for swap refunds and payouts |
| `history(limit)` | Transactions, newest first, with recipients and memos; a send to yourself is its own kind |
| `propose_send(&SendRequest)` | Propose a payment and build its PCZT |
| `propose_shield(compact)` | Propose moving every transparent coin into the shielded pool |
| `finish(unsigned, zsig, broadcast, on_event)`, `finish_files(..)` | Apply the signatures, prove, extract, store, broadcast |
| `store_transaction(raw, height)` | Store a transaction made elsewhere |
| `check_address(network, addr)` | Whether this wallet can pay `addr` on `network` (unified, Orchard-only unified or transparent; not Sapling) |
| `tip(server)`, `check_chain`, `broadcast_on` | Server info, the chain check, broadcast |
| `parse_zec`, `format_zec`, `short_fingerprint` | Amounts and the ZFP shown on the board |

### Sync and transparent spends

Compact blocks carry no transparent inputs, so a scan alone never sees a
transparent coin of the wallet being spent by a transaction it did not make,
for example a shield made by another wallet with the same keys (`zecz` on
another machine, the app, the Chrome extension). The coin would stay in the
balance for good.

So `sync` ends with `fetch_transaction_data`. It answers librustzcash's
transaction data requests (full transactions to enhance, the status of
unmined ones, the transactions of t-addresses), then asks the server for the
UTXO set of every t-address that still holds an unspent coin in the wallet.
A coin missing from that set was spent: the t-address's transactions since
the coin was mined are stored, which records the spend. A request that
fails is counted and asked again on the next sync. The chain side is the
`lwd::ChainData` trait, implemented for the lightwalletd client and by an
in-memory chain in the tests.

### Propose, sign, finish (the PCZT flow)

```
propose_send / propose_shield ──▶ SendProposal { signer_view, sha256, payments, change, fee }
                                  full PCZT kept in pending/<sha256>.pczt
          signer_view ──▶ the board (or the DEV soft signer) ──▶ .zsig
finish(signer_view, zsig) ──▶ check the .zsig names sha256(signer_view)
                              same sighash as the pending full PCZT
                              apply each signature (Signer role: verified against its key)
                              prove (Orchard / Ironwood), extract, store, broadcast
```

- **Signer view.** `SendRequest { to, amount_zat, memo, compact }`. With
  `compact`, the board gets the compact view (v2 PCZT, about 1.4 KB): no
  proofs, and the fields the board can rebuild (note and value commitments,
  memo ciphertext) left out. Otherwise the full view, redacted for the
  signer.
- **Pools.** Change goes to the Orchard-protocol pool current at the target
  height: Ironwood once NU6.3 is active, Orchard before. A shield goes there
  too. Coins below the ZIP 317 marginal fee are left as dust, and a
  transparent balance that would all go to the fee is refused.
- **Signatures.** `authorize::apply_zsig` applies RedPallas signatures to
  Orchard and Ironwood actions, and, for a version 2 `.zsig`, the ECDSA
  signature and key of each transparent input, each checked against its
  ZIP 244 sighash and the key its script pays to. Sapling spends are
  refused.
- **Proving.** `authorize::prove_with` loads the proving keys and proves
  each bundle, with progress events.

### Seed cards and dev tools

| Module | What |
|---|---|
| `zkv` | The `.zkv` format: `seal`, `open`, `peek` (byte for byte with the firmware's `zkv.c`) |
| `seed_card` | `write_new` and `describe` for the CLI's `zkv-new` and `zkv-open` |
| `keys::DevAccount`, `soft_signer` | DEV ONLY: keys from a phrase, and a signer that writes a `.zsig` without any check. For tests on testnet with public phrases |
| `device`, `zsig` | Re-exported from [`leek-device`](../leek-device/README.md) |

## Features

`serial` (default) turns on the desktop serial port in `leek-device`
(`Device::open`). Turn it off for Android, where the transport comes from
elsewhere.

## Tests

```sh
cd host
cargo test -p zecz-core                  # offline
cargo test -p zecz-core -- --ignored     # also the testnet and mainnet network tests (read only, no broadcast)
```

| File | What |
|---|---|
| `tests/wallet.rs` | Profiles, importing the public test account (abandon x23 art) with an offline birthday, addresses against `vectors/keys.json`, the refusals of `send` and `finish`, `.zkv` cards |
| `tests/sign_soft.rs` | The soft signer against the committed PCZT vectors |
| `tests/device_zdev.rs` | The board path with no board: drives [zdev](../../firmware/sim/README.md) as `zecz sign-device` does, then proves and extracts. Builds zdev with `make`, skips loudly if it cannot |
| `tests/shield.rs` | `propose_shield` on a wallet with one transparent coin |
| `tests/transparent_spend.rs` | A transparent coin spent by another wallet, found by `fetch_transaction_data` against an in-memory chain |
| `tests/history.rs` | The history of a real transaction from the vectors, on both sides |
| `tests/receive.rs` | Rotating addresses and the address check |
| `tests/diversified.rs` | Diversified-address vectors for the firmware's address check |
| `tests/mainnet.rs` | Mainnet: coin type 133, mainnet vectors, the refusals that keep the networks apart |
| `tests/zkv_golden.rs` | `.zkv` against the firmware's golden file |
