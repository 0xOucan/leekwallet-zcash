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
src/account.rs    seed_fingerprint, get_account, show_address, set_birthday, get_features; Network, Account, Features
src/preflight.rs  preflight: host-side checks before anything is sent
src/prepare.rs    prepare_for_signer: ZIP 32 / BIP 32 derivations some wallets leave out
src/redact.rs     signer_view_bytes: upstream redact_pczt_for_signer
src/apply.rs      apply_zsig: .zsig -> signed PCZT, every signature verified
src/sign.rs       sign_pczt, SignOptions, progress, CancelToken / CancellablePort
src/ua.rs         to_checkable_ua
src/sd.rs         prepare_sd_sign, finish_sd_sign: the SD-card air gap (experimental)
src/record.rs     AccountRecord: the import record as CBOR
src/persist.rs    SignResult::to_bytes / from_bytes, txid, check_mined
src/storage.rs    get_storage_key, StorageKey: App Storage Keys, subkeys, seal / open
src/quick.rs      QuickUnlock, KeyWrapper: the Storage Key behind the platform's biometric or PIN
src/error.rs      LeekError (stable codes, Recovery), firmware code mapping
```

## API

| Call | What it does |
|---|---|
| `get_features(&mut Device) -> Features` | `getFeatures` + `zGetFeatures`: firmware, model, spend pools, branch IDs, tx versions, max PCZT size, max transparent inputs (16) |
| `seed_fingerprint(&mut Device, Option<Network>) -> [u8; 32]` | `zGetSeedFingerprint`: the ZIP 32 seed fingerprint of the seed open on the board, with **no question on the board**. Use it on every connect to recognise the board (see "Recognising the board"); `FirmwareTooOld` on firmware without it |
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
| `get_storage_key(&mut Device, app_id, index, Network) -> StorageKey` | `zGetStorageKey`: the app's Storage Key, after the user presses OK on the board, every time (below) |
| `get_storage_key_with(.., progress) -> StorageKey` | the same, calling `progress(AccountProgress::ConfirmOnDevice)` first |
| `StorageKey::{seal, open, seal_db, open_db, seal_ufvk, open_ufvk}` | XChaCha20-Poly1305 under the label's subkey, with a key check; `open` tells a wrong key (`StorageKeyMismatch`) from damage (`StorageCorrupted`) |
| `StorageKey::{subkey, to_bytes, from_bytes, from_seed, forget}` | a subkey (`db` for SQLCipher), the key for secure storage and back, the key from a seed (tests and emergency recovery only), wipe now |
| `QuickUnlock::{create, from_bytes, to_bytes, open, disable}` | quick unlock: the Storage Key wrapped by the app's `KeyWrapper` (hardware-backed, user-authenticated platform key); `app_id`, `account`, `network`, `label` read the record (below) |
| `validate_app_id(app_id)` | the board's App identifier rules, checked on the host first |
| `classify(&anyhow::Error) -> LeekError` | maps `leek-device` errors (for example from `Device::pair`) |

`SignOptions { view, progress, cancel, features, verify_account }`:
`progress` receives `Checking`, `Uploading { sent, total }`,
`ReviewOnDevice`, `Applying`. `cancel` is a `CancelToken` checked between
steps; wrap the port in `CancellablePort` with the same token to interrupt an
upload or a wait too (the session must then be re-paired). During the review
the user cancels on the board itself. `verify_account` asks the board which
seed it holds first (`seed_fingerprint`, no question) and refuses a different
seed before anything is uploaded.

### Errors

One `LeekError` for everything, `#[non_exhaustive]`. `code()` is stable (1xx
link, 2xx session and board, 3xx user, 4xx account and network, 5xx PCZT,
6xx signatures, 7xx stored app data and quick unlock (706 to 709, see "Stored app data"), 900 an unknown firmware code; `DuplicateRk`, the board's
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
that). `sign_pczt` with `verify_account` reads only the seed fingerprint, so
it does not ask (on firmware without `zGetSeedFingerprint` it reads the
account, and asks only if this session has not shared it yet).

## Recognising the board

A wallet that has imported the account already has its UFVK; on the next
connect it only needs to know which seed the board has open (the same board,
another one, or a passphrase wallet on it). Call `seed_fingerprint` for
that, and compare it with the `seed_fingerprint` stored at import: the board
answers it after pairing and unlock with no question, where `get_account`
would put "Share view key?" up on every connect. Call `get_account` when the
UFVK is actually needed (the first import, or a new account). With a
network, `seed_fingerprint` also gets `WrongNetwork` from a board set to the
other one; the fingerprint itself does not depend on the network or the
account.

The fingerprint is a **stable identifier of the seed that every paired host
can read**: the same 32 bytes in every app, session and network, for as long
as the seed exists. It reveals no key or address (and the board shows its
first four bytes, "ZFP 1A2B3C4D", on its own screen), but two hosts that
compare notes can tell they saw the same wallet. Treat it as you would an
account identifier, not as a secret and not as an anonymous one.

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

// Import once: the same account record a Keystone import makes. The board
// asks "Share view key?" here.
let account = get_account(&mut dev, 0, Network::Main)?;
wallet.import_account_ufvk(&account.ufvk, account.seed_fingerprint, account.index, KEY_SOURCE, account.birthday);

// Every later connect: which seed, with no question on the board.
if seed_fingerprint(&mut dev, Some(Network::Main))? != account.seed_fingerprint {
    tell_user_another_seed_is_open();
}

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
- On the board's side: it asks "Share view key?" on its screen before
  releasing the viewing key, and the six-digit pairing code is required on
  every connection.

This crate and its bindings never write a UFVK anywhere; there is no API
that persists one in plaintext.

## Stored app data: three tiers

An app can protect what it stores in one of three ways. The app vendor
chooses which to offer (any subset, with a setting, which it may hide). All
three encrypt the database; they differ in what opens it.

| Tier | Opens with | Protects a stolen device with |
|---|---|---|
| **Unlock with the board** | the board: pair, "Unlock app data?", OK, every time | the board, its PIN and a press on it; the host holds ciphertext only |
| **Quick unlock** | the user's fingerprint, face or device PIN, after one unlock with the board | the platform's secure hardware (StrongBox or TEE, Secure Enclave, TPM, a passkey) and its attempt limits |
| **Remember on this device** | nothing beyond the host's lock screen | the host's lock screen and secure storage |

**Unlock with the board.** The app keeps nothing readable. Its database and
its copy of the UFVK are sealed under the app's **Storage Key**
(docs/zcash/zip-draft-app-storage-keys.md), which only the board can produce:

1. First connection: pair, `get_account` (the user shares the view key),
   then `get_storage_key(app_id, account, network)` (the user approves
   "Unlock app data?"). Seal the UFVK with `seal_ufvk` and the database with
   `seal_db` (or key SQLCipher with `subkey("db")`), store the sealed bytes,
   and drop the `StorageKey` when the app closes.
2. Every later opening: pair, `get_storage_key`, the user approves on the
   board, `open_ufvk` / `open_db`, sync. The board asks every time; it
   remembers no yes, not even for the session.
3. A new phone or a new board: the same seed (and passphrase) on any board
   gives the same key, so a copy of the sealed files opens again.

**Quick unlock.** The same sealed data, but the Storage Key is also kept
wrapped by a platform key that lives in secure hardware and works only after
the user's biometric or device PIN. The user sees balances and syncs without
the board; spending still needs it.

1. Turn it on (after an unlock with the board, holding the `StorageKey`):
   `QuickUnlock::create(&key, &wrapper)` with the app's `KeyWrapper`, and
   store `to_bytes()` in the app's private storage.
2. Every later opening: `QuickUnlock::from_bytes(stored)?.open(app_id,
   account, network, &wrapper)` shows the platform's prompt and returns the
   `StorageKey`; then `open_ufvk` / `open_db` as above.
3. Turn it off: `QuickUnlock::disable(&wrapper)` (deletes the platform key)
   and delete the stored record. The board path is untouched.

A `KeyWrapper` is the app's: `info()` (`hardware_backed`,
`user_auth_required`, a `label` for the unlock screen), `wrap(key)`,
`unwrap(blob)` and `delete_key()`. Reference implementations for Android
Keystore, the Apple Secure Enclave and WebAuthn PRF are in
[`leek-zcash-ffi/examples/quick-unlock`](../leek-zcash-ffi/examples/quick-unlock).
`create` refuses a wrapper that does not report **both** flags
(`QuickUnlockNotSecure`, 706) before it is called, and so does `open`.

**No software PIN.** There is no software wrapper, and an app must not write
one: a key derived on the host from a 4- to 6-digit PIN is at most a million
guesses, which a copy of the app's files exhausts offline: in seconds with a
fast hash, in hours to days even with a slow, memory-hard one. The attempt
limits that make a short PIN safe exist only in hardware. On a host
without a hardware-backed, user-authenticated key (an old phone, a desktop
without a TPM or Secure Enclave, a browser without passkey PRF), do not offer
quick unlock; offer the board, remember-on-this-device, or (documented as
weaker) a long passphrase stretched with a memory-hard KDF.

What `open` returns, in order: `StorageKeyMismatch` (702) for a record of
another app, account or network, before any prompt; `QuickUnlockNotSecure`
(706); the wrapper's failure, `QuickUnlockCancelled` (708, the user closed
the prompt: stay locked, offer it again or the board), `QuickUnlockInvalidated`
(707, the platform key is gone: a new fingerprint enrolled, the device PIN
removed, the app reinstalled) or `QuickUnlockFailed` (709); then
`QuickUnlockInvalidated` if the unwrapped key fails the key check, and
`StorageCorrupted` (703) if any byte of the record changed. After anything
but a cancel, offer "Unlock with the board" and set quick unlock up again
once it succeeds.

The record (`QuickUnlock::to_bytes`):

```
"ZAQU" || 0x01 (version) || 0x03 (flags: hardware-backed, user auth)
       || coin_type (LE32) || account (LE32) || [len] app_id || [len] label
       || salt (32) || check (32) || wrapped_len (LE16) || wrapped || tag (32)
```

`check = HMAC-SHA256(SubKey("commit"), salt)`, the draft's key check (a
wrong unwrap fails it); `tag = HMAC-SHA256(SubKey("commit"),
"Zcash_AppStorage_QuickUnlock_v1" || every byte before it)`, so the record
cannot be moved to another app, account or network or changed in any byte.
`wrapped` is the platform's own authenticated encryption (at most 4096
bytes). The record is not secret without the platform key, but keep it in
the app's private storage, out of cloud backups (it is useless on another
device anyway).

**Remember on this device.** The rule above: the app keeps the UFVK (or the
Storage Key, `StorageKey::to_bytes` / `from_bytes`) in the platform's secure
storage and opens without the board or a prompt. The data is as safe as the
phone's or computer's lock screen and secure storage.

The Storage Key is per app identifier, network and account, unrelated to
every spending and viewing key and to other apps' keys. The app identifier is
lower-case ASCII reverse-DNS (`com.example.wallet`), the same on every
platform and in every version that should open the same data. The board
shows it in full; `validate_app_id` applies its rules on the host first.
`Features::storage_key` says whether the board has the call (and under which
ZIP number: 65535, a placeholder, until the draft is assigned one; every key
changes with it).

What each tier protects, said plainly:

| | Unlock with the board | Quick unlock | Remember on this device |
|---|---|---|---|
| Host lost or stolen, locked | protected: ciphertext only | protected by the secure hardware and its attempt limits | protected by the host's lock screen and secure storage |
| Disk image or cloud backup | protected: ciphertext only | protected: the platform key does not leave the device | protected if the secure storage is not in the backup |
| Someone who knows the host's PIN or password | needs the board, its PIN and a press on it | **can open the app** (the device PIN unlocks it) | can open the app and read everything |
| A malicious app, or malware on the host while the app is open | **not protected**: an open app holds the key, the UFVK and the data | **not protected** | **not protected** |
| The board's seed | unaffected: the key reveals nothing about it | unaffected | unaffected |

No tier protects against the app itself or against malware running as the
user while the app is open, and none can take back a viewing key that was
shared: whoever saw the UFVK can watch the account for good. A user who
thinks the account is being watched should move the funds to a new seed (a
new wallet on the board, a fresh backup) and set the app up again on it.
That is the only remedy.

How to present the choice: name the tiers by what the user does, not by
keys. For example "Open with the board each time (most private)", "Open with
fingerprint or PIN (after the board once)" and "Remember on this phone
(opens without the board)". Say what each trades away, make every tier but
the first an explicit choice, and let the user switch back, which deletes
what was remembered (`QuickUnlock::disable` and the record). Offer quick
unlock only where `info()` reports a hardware-backed, user-authenticated key,
and keep "Use the board" on its prompt. Ask the board for the Storage Key
only right after the user opened the app: the board cannot tell which app is
asking, and the user's press is the protection. In "unlock with the board"
mode show "Confirm on the board" for every open (`get_storage_key_with`
reports `ConfirmOnDevice`), and handle `UserRejected` as "stay locked".

Sealed objects (`seal`) are this crate's format: `"ZASK" || 0x01 (version)
|| 0x01 (XChaCha20-Poly1305) || salt (32) || check (32) || nonce (24) ||
ciphertext || tag (16)`, with `check = HMAC-SHA256(SubKey("commit"), salt)`,
a random salt and nonce per seal, and associated data the header followed by
the app identifier, coin type, account and label. A different seed, app,
account or network fails the key check (`StorageKeyMismatch`) before
anything is decrypted; any changed byte fails (`StorageCorrupted`). Keys and
subkeys are wiped on drop; `forget` wipes at once.

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
- `src/quick.rs` unit tests, with a fake platform key: round trip through
  the stored bytes (the key opens what it sealed), refusal of wrappers that
  are not both hardware-backed and user-authenticated (before `wrap`, and on
  `open`), another app, account or network refused before any prompt, a
  record rewritten for another app, account or network refused by its tag,
  every single-bit change refused, a wrapper returning another or a short
  key, a cancelled prompt, another platform key, `disable`, a forgotten key,
  and a pinned record layout.
- `src/storage.rs` unit tests: the draft's four Storage Key vectors and their
  `db` / `ufvk` / `commit` subkeys, the draft's ZIP 32 expansion written out
  by hand, App identifier rules, seal / open round trips, a pinned sealed
  format, every single-bit change refused, another seed, app, account,
  network or label refused, labels, `forget`.
- `tests/zdev.rs`: against the simulated board (built with `make` if needed,
  skipped loudly otherwise): features, account, address check (verified, not
  ours, rejected), birthday, `sign_pczt` in both views with transparent inputs,
  a rejection on the board, host-side refusals, a board-side refusal, a
  cancel mid-upload, and the Storage Key (refused, then released on each
  press, equal to the SDK's own derivation, opening what an earlier release
  sealed).

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
