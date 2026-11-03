# `leek-zcash`: the LeekWallet SDK's Zcash kit

The layer a Zcash wallet links to sign with a LeekWallet board through the
hardware-signer path it already has for Keystone or Ledger. A **signed
PCZT** comes out, so the wallet combines, proves and extracts exactly as it
does for Keystone (Zodl `createTransactionFromPczt`, Noir
`finalizeSignedPczt`); the board's `.zsig` stays available for audit and for
the SD-card flow.

It sits on [`leek-device`](../leek-device/README.md) (pairing, the encrypted
session, the `z*` calls, `.zsig`) and has no transport of its own: every
device call takes a `Device`, which runs over any `FramePort` (USB serial,
Bluetooth, a Kotlin or JavaScript callback, the zdev simulator). The PCZT
side is plain functions over bytes and `pczt::Pczt`, built on the same
librustzcash commit as the rest of `host/`.

```
src/lib.rs        the public API (re-exports) and crate docs
src/account.rs    get_account, show_address, set_birthday, get_features; Network, Account, Features
src/preflight.rs  preflight: host-side checks before anything is sent
src/prepare.rs    prepare_for_signer: ZIP 32 / BIP 32 derivations some wallets leave out
src/redact.rs     signer_view_bytes: upstream redact_pczt_for_signer
src/apply.rs      apply_zsig: .zsig -> signed PCZT, every signature verified
src/sign.rs       sign_pczt, SignOptions, progress, CancelToken / CancellablePort
src/ua.rs         to_checkable_ua
src/sd.rs         prepare_sd_sign, finish_sd_sign: the SD-card air gap (experimental)
src/record.rs     AccountRecord: the import record as CBOR
src/persist.rs    SignResult::to_bytes / from_bytes, txid, check_mined
src/error.rs      LeekError (stable codes, Recovery), firmware code mapping
```

## API

| Call | What it does |
|---|---|
| `get_features(&mut Device) -> Features` | `getFeatures` + `zGetFeatures`: firmware, model, spend pools, branch IDs, tx versions, max PCZT size, max transparent inputs (16) |
| `get_account(&mut Device, index, Network) -> Account` | `zGetAccount`: UFVK (Orchard + P2PKH), UA, t-address, seed fingerprint, birthday, after the user presses OK on the board (once per session). Import it with `KEY_SOURCE` (`"leekwallet"`) |
| `get_account_with(&mut Device, index, Network, progress) -> Account` | the same, calling `progress(AccountProgress::ConfirmOnDevice)` before the board may ask |
| `show_address(&mut Device, index, Network, ua) -> AddressCheck` | `zShowAddress`: `Verified`, `Rejected`, `NotOurs` or `Unsupported` |
| `to_checkable_ua(ua) -> String` | the same UA without its Sapling receiver, which the board cannot check |
| `set_birthday(&mut Device, index, Network, height) -> bool` | `zSetAccountMeta`; true if kept in flash |
| `preflight(&Pczt, &AccountMeta, &Features)` | refuses on the host what the board would refuse, with a precise error |
| `prepare_for_signer(Pczt, ufvk, seed_fp, index) -> Pczt` | fills missing ZIP 32 (shielded spends) and BIP 32 (transparent inputs) derivations for this account |
| `signer_view_bytes(&Pczt, SignerView) -> Vec<u8>` | what goes to the board: `redact_pczt_for_signer`, `Full` or `Compact` |
| `sign_pczt(&mut Device, &Pczt, &AccountMeta, SignOptions) -> SignResult` | preflight, view, upload, review on the board, verify and apply. `SignResult { signed_pczt, signatures, sighash, zsig }` |
| `apply_zsig(&Pczt, sent, zsig) -> Pczt` | a `.zsig` (from any transport) into the wallet's PCZT |
| `prepare_sd_sign(&Pczt, &AccountMeta, SignerView) -> SdRequest` | **experimental**: preflight and the signer view for the card. `SdRequest { file_name: "LEEKZ/unsigned.pczt", bytes, sent_hash, account, network }` |
| `finish_sd_sign(&Pczt, sent, zsig) -> SignResult` | **experimental**: the board's `LEEKZ/signed.zsig` (hash checked against `sent`, the request or its bytes) into the same `SignResult` as `sign_pczt` |
| `AccountRecord::{encode, decode, from_accounts}` | one seed's accounts as CBOR (below) |
| `SignResult::{to_bytes, from_bytes, txid_hex}` | persist the signed result and its txid before broadcast |
| `check_mined(signed_tx, mined_tx) -> MinedCheck` | `Identical`, `SameEffectsOtherAuth` (same txid, other proofs or signatures) or `Different`; `tx_txid(tx)` |
| `classify(&anyhow::Error) -> LeekError` | maps `leek-device` errors (for example from `Device::pair`) |

`SignOptions { view, progress, cancel, features, verify_account }`:
`progress` receives `Checking`, `Uploading { sent, total }`,
`ReviewOnDevice`, `Applying`. `cancel` is a `CancelToken` checked between
steps; wrap the port in `CancellablePort` with the same token to interrupt an
upload or a wait too (the session must then be re-paired). During the review
the user cancels on the board itself. `verify_account` asks the board for the
account first and refuses a different seed before anything is uploaded.

### Errors

One `LeekError` for everything, `#[non_exhaustive]`. `code()` is stable (1xx
link, 2xx session and board, 3xx user, 4xx account and network, 5xx PCZT,
6xx signatures, 900 an unknown firmware code; `DuplicateRk`, the board's
`ZS_E_DUPLICATE_RK` (0x0518), is 517). `recovery()` says
`RetrySameLink`, `Reconnect` or `DoNotRetry`. Every firmware code in
`docs/PROTOCOL.md` §6, `ZS_E_*` and `ZSIGN_E_*` is mapped (a few by call:
code 0x0E is `AddressNotOurs` for an address check and `OutputMismatch` for
signing). The message is a fixed sentence per variant: it never contains an
amount, an address, a key, PCZT bytes or the board's free-text message, so a
wallet can forward it to logs and analytics.

## The viewing key

The UFVK is released only after a press on the board. It cannot spend, but
it shows the account's whole balance and history, so the board asks "Share
view key?" (naming the account, the network and the session's pairing code)
and answers `get_account` only after OK. A yes holds for the rest of that
session, for that account, network and seed; a new pairing asks again, and
nothing is remembered across a reboot. Cancel on the board is
`UserRejected`, no answer within its 120 s `UserTimeout`; the call waits for
both. The board does not say whether it is asking, so show "Confirm on the
board" for every call (`get_account_with` reports `ConfirmOnDevice` for
that). `sign_pczt` with `verify_account` reads the account too, and asks only
if this session has not shared it yet.

## What the host checks

The board checks everything itself (sighash computed on the board; cmx, cv
and rk recomputed; note encryption checked; change hidden only when its own
keys derive it; transparent inputs signed only at its fingerprint and path;
branch ID from a known list). The host adds:

Before sending (`preflight`):
- the PCZT's coin type is the account's network;
- its transaction version and consensus branch are in the board's lists;
- no Sapling spend or output;
- no two Orchard or Ironwood actions, both pools together, share an `rk`
  (raw bytes, signed padding included, as the board and Keystone refuse it);
- at most 16 transparent inputs, each plain P2PKH with `SIGHASH_ALL`, no
  existing signatures, exactly one BIP 32 derivation naming this seed
  fingerprint, coin type and account at scope 0, 1 or 2, whose key the script
  pays to and (given the UFVK) the account derives;
- every unsigned Orchard / Ironwood spend: its ZIP 32 derivation, if any,
  names this seed and account; given the UFVK, its nullifier and `rk` derive
  from the account's Orchard key; no dummy key; a spend another signer already
  signed carries no value;
- the compact view fits the board's maximum PCZT size.

After the board answers (`apply_zsig`, `sign_pczt`):
- the `.zsig` is the SHA-256 of the exact bytes sent;
- the bytes sent are the same transaction as the wallet's PCZT (the same
  shielded sighash, which commits to every input and output), and the sighash
  the board reviewed is the host's;
- every signature verifies before it is stored: Orchard and Ironwood against
  the action's `rk` and the shielded sighash, transparent against the input's
  own ZIP 244 sighash and the key its script pays to (upstream
  `Signer::apply_orchard_spend_auth_signature` and
  `append_transparent_signature`);
- no duplicate, no signature for an action that was already signed, nothing
  left unsigned afterwards.

## How a wallet uses it

A Zodl- or Noir-style integration, next to its Keystone path:

```rust
use leek_zcash::*;

// Connect (the wallet's own FramePort), pair, unlock: leek-device.
let mut dev = leek_device::device::Device::with_port(Box::new(port));
dev.pair(|code| show_pairing_code(code)).map_err(|e| classify(&e))?;
dev.ensure_unlocked().map_err(|e| classify(&e))?;

// Import once: the same account record a Keystone import makes.
let account = get_account(&mut dev, 0, Network::Main)?;
wallet.import_account_ufvk(&account.ufvk, account.seed_fingerprint, account.index, KEY_SOURCE, account.birthday);

// Receive: let the user confirm the address on the board.
match show_address(&mut dev, 0, Network::Main, &to_checkable_ua(&shown_ua)?)? {
    AddressCheck::Verified => {}
    _ => warn_user(),
}

// Send: the wallet builds and keeps its PCZT as for Keystone ...
let pczt = wallet.create_pczt_from_proposal(..)?;
// ... (zkool2-style builders: pczt = prepare_for_signer(pczt, &ufvk, &fp, 0)?) ...
let r = sign_pczt(&mut dev, &pczt, &account.meta(), SignOptions::default())?;
// ... and hands the signed PCZT to the path Keystone's takes.
wallet.extract_and_store_transaction_from_pczt(r.signed_pczt)?;
```

## SD card (experimental)

The board's SD flow (`firmware/src/sd-sign.c`, `docs/zcash/FORMATS.md`
"SD-card airgap layout") reads `/LEEKZ/unsigned.pczt`, checks it exactly
as a USB upload, shows the same review, hashes the file again before
signing (a card changed under the review is not signed) and writes
`/LEEKZ/signed.zsig`, whose hash field is the SHA-256 of the file it read.
It signs for the account and network selected on the board.

```rust
let req = prepare_sd_sign(&pczt, &account.meta(), SignerView::Full)?;
// write req.bytes to <card>/LEEKZ/unsigned.pczt, delete any old signed.zsig,
// keep req (or req.bytes) with the wallet's PCZT.
// The user selects account req.account on req.network, then
// SD Card > Sign transaction on the board.
let r = finish_sd_sign(&pczt, &req, &std::fs::read("<card>/LEEKZ/signed.zsig")?)?;
```

A `.zsig` left from another transaction is `ZsigHashMismatch`. The optional
`unsigned.json` (display only) is not written; the board does not read it.
Experimental: the card layout may still change with the firmware's SD
code.

## Account record

`AccountRecord { seed_fingerprint, accounts: [AccountEntry { ufvk, index,
name, birthday }] }`, type name `leek-zcash-accounts`
(`ACCOUNT_RECORD_UR_TYPE`). The CBOR body has the shape wallets already
parse for a hardware signer's Zcash accounts (Keystone's `zcash-accounts`):
`{1: bytes(32) seed fingerprint, 2: [{1: UFVK, 2: index, ?3: name}]}`, plus
our key 4 (birthday height), which other readers skip. Account maps are
written untagged and read tagged or untagged; unknown keys are skipped; a
bad UFVK, an index of 2^31 or more, or trailing bytes are `Parse`. It holds
viewing keys: store it as the next section says.

## Persist before broadcast

From the Konclave lessons (`wallets/notes/konclave-arweave.md`): a wallet
that loses a signed transaction between signing and broadcast cannot tell
whether it went out, and re-signing may spend other notes.

1. **Persist before broadcast.** `SignResult::to_bytes()` (signed PCZT,
   `.zsig`, sighash and txid: ZIP 244's txid commits to the effects only, so
   it is final before proving) goes to durable storage before anything is
   sent. The record ends in a SHA-256 checksum; `from_bytes` checks it,
   re-derives the txid and sighash, and refuses a damaged record.
2. **Broadcast idempotently.** Retry with the same transaction (same txid)
   until it is mined or expires; never re-sign the same intent while a txid
   is pending.
3. **After mining, fetch and compare.** `check_mined(&broadcast, &mined)`:
   `Identical`, `SameEffectsOtherAuth` (the approved payment, proved or
   signed again) or `Different` (not the wallet's transaction).

## Host-side protection

The board keeps the spending key; the host keeps the viewing key and the
wallet's history, and both deserve protection:

- Keep the UFVK (and the `AccountRecord`, which contains it) in the
  platform's secure storage: Android Keystore, the iOS / macOS Keychain,
  Windows DPAPI / Credential Manager, libsecret on Linux.
- Encrypt the wallet database too. Notes, history and memos already reveal
  what the user did; encrypting only the key is not enough.
- Be honest about what this buys: it protects a stolen device or a backup,
  not a malicious app running as the user. The board cannot prevent a host
  it is paired with from storing the viewing key.
- On the board's side: a coming firmware will ask on its screen before
  releasing the viewing key, and the six-digit pairing code is required on
  every connection.

This crate and its bindings never write a UFVK anywhere; there is no API
that persists one in plaintext.

## Tests

```sh
cargo test -p leek-zcash
```

- `src/` unit tests: error codes unique, messages free of data, firmware code
  mapping, `to_checkable_ua`.
- `tests/apply.rs`: the vector `.zsig`; soft-signer signatures over both
  views of shielded, transparent and mixed PCZTs apply and the transactions
  prove and extract; a `.zsig` for other bytes, another transaction, a bad
  signature (shielded and transparent), a wrong pool or index, duplicates,
  missing signatures and malformed files are each refused with their error;
  upstream's compact view equals the vectors leek-android's `compact_view`
  made.
- `tests/preflight.rs`: every vector the board signs passes; the refusal
  vectors (other network, old branch, a repeated `rk` within and across
  pools, foreign spend, transparent inputs with
  missing, foreign, wrong-account or wrong-path derivations, `SIGHASH_NONE`)
  get their precise error; limits.
- `tests/prepare.rs`: stripped and zero-fingerprint derivations are restored
  exactly; signatures over the prepared PCZT apply to the unprepared one.
- `src/record.rs` unit tests: the account record round-trips, has the
  shared body shape, reads tagged accounts and unknown keys, refuses
  malformed records and accounts of two seeds.
- `tests/sd.rs`: the card file is the signer view and its hash; the soft
  signer's `.zsig` (what the board writes) finishes into a `SignResult`
  whose txid equals the extracted transaction's; a stale card, another view
  or another transaction is refused; preflight runs before the file is
  made; `SignResult` round-trips through bytes and refuses damage;
  `check_mined` tells identical, re-extracted and different transactions
  apart.
- `tests/zdev.rs`: against the simulated board (built with `make` if needed,
  skipped loudly otherwise): features, account, address check (verified, not
  ours, rejected), birthday, `sign_pczt` in both views with transparent inputs,
  a rejection on the board, host-side refusals, a board-side refusal, and a
  cancel mid-upload.

## Bindings

- Kotlin and Swift: [`leek-zcash-ffi`](../leek-zcash-ffi/README.md)
  (UniFFI, proc-macro): the byte API, `LeekError` by category with its
  stable `code` and `recovery`, a foreign `FrameTransport` (the app owns
  USB or Bluetooth), progress and cancel. Generated bindings are in that
  crate.
- Next: `wasm-bindgen` for TypeScript (Noir): the same byte API,
  single-threaded, a JS `FramePort` over Web Serial / Web Bluetooth made
  async with a sans-IO session (the current `Device` blocks in `recv`).
  `zcash_client_backend` is a heavy dependency for one function; for WASM
  either feature-gate it upstream or carry the redaction behind a feature.
- Then: plain types for flutter_rust_bridge (zkool2, Vizor); every
  transport behind a feature flag; a testing package with a mock port, the
  zdev port and the vectors.

Also left for next slices: L2 device actions as a state stream
(`ConfirmPairingCode`, `UnlockOnDevice`, `ReviewOnDevice`, ...);
re-checking expiry against the chain height after the board returns; typed
transport errors from `leek-device` instead of matching its wording;
replacing `leek-android`'s hand-written `compact_view` with this crate (the
vectors show they are byte-identical).
