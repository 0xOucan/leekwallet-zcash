# `leek-zcash-ffi`: Kotlin and Swift bindings for the LeekWallet Zcash kit

[`leek-zcash`](../leek-zcash/README.md) through [UniFFI](https://mozilla.github.io/uniffi-rs/)
(`=0.29.5`, proc-macros, no UDL), for Android (Kotlin) and iOS / macOS
(Swift). A `cdylib` and a `staticlib`.

```
src/lib.rs                      the exported API
src/bin/uniffi-bindgen.rs       the generator, same uniffi as the scaffolding
uniffi.toml                     Kotlin package io.leekwallet.zcash.sdk, Swift module LeekZcash
scripts/generate.sh             regenerates bindings/ from the built library
scripts/kotlin-smoke.sh         compiles bindings/kotlin + tests/kotlin/Smoke.kt and runs it on the JVM
bindings/kotlin/                generated: io/leekwallet/zcash/sdk/leek_zcash_ffi.kt
bindings/swift/                 generated: LeekZcash.swift, LeekZcashFFI.h, LeekZcashFFI.modulemap
tests/ffi.rs                    the FFI layer from Rust, against the zdev simulator
examples/quick-unlock/          reference KeyWrappers: Android Keystore (Kotlin), Secure Enclave (Swift), WebAuthn PRF (JS)
```

## API

Byte-oriented: PCZTs, `.zsig` files, account records and signed results
cross as bytes (`ByteArray`, `Data`); 32-byte values (seed fingerprint,
sighash, txid) are bytes of length 32. Names below are Rust; Kotlin and
Swift use camelCase.

| Call | |
|---|---|
| `preflight(pczt, AccountMeta, Features?)` | host-side checks |
| `prepare_for_signer(pczt, ufvk, seed_fingerprint, index) -> pczt` | missing derivations |
| `signer_view_bytes(pczt, SignerView) -> bytes` | what goes to the board |
| `apply_zsig(pczt, sent, zsig) -> signed pczt` | every signature verified |
| `to_checkable_ua(ua) -> ua` | without the Sapling receiver |
| `prepare_sd_sign(pczt, AccountMeta, SignerView) -> SdRequest` | **experimental**: `{file_name, zsig_file_name, bytes, sent_hash, account, network}` |
| `finish_sd_sign(pczt, sent, zsig) -> SignResult` | **experimental** |
| `sign_result_from_bytes(record) -> SignResult` | read a persisted result back |
| `check_mined(signed_tx, mined_tx) -> MinedCheck`, `tx_txid(tx)` | after mining |
| `encode_account_record(AccountRecord)`, `decode_account_record(bytes)`, `account_record_type()` | the `leek-zcash-accounts` record |
| `LeekDevice(FrameTransport, LinkKind)` | a board over the app's link |
| `.pair(PairingListener)`, `.ensure_unlocked()` | the six-digit code goes to the listener |
| `.seed_fingerprint(network?)` | which seed the board holds (32 bytes), with no question on the board: call it on every connect, `get_account` only when the UFVK is needed. A stable identifier of the seed that every paired host can read |
| `.get_features()`, `.get_account(index, network)`, `.show_address(..)`, `.set_birthday(..)` | |
| `.sign_pczt(pczt, AccountMeta, SignerView, verify_account, SignListener?) -> SignResult` | progress: `Checking`, `Uploading{sent,total}`, `ReviewOnDevice`, `Applying` |
| `.get_storage_key(app_id, index, network) -> StorageKey` | the app's Storage Key, after "Unlock app data?" is answered OK on the board, every time |
| `StorageKey.from_bytes(key, app_id, account, network)` | a key the app kept in secure storage ("remember on this device") |
| `StorageKey.seal_db / open_db / seal_ufvk / open_ufvk / seal / open` | sealed app data (XChaCha20-Poly1305, key check); `db_key()` / `subkey(label)` for SQLCipher; `key_bytes()` for secure storage only; `forget()` wipes it now |
| `QuickUnlock.create(StorageKey, KeyWrapper)`, `QuickUnlock.from_bytes(record)` | quick unlock: wraps the key with the app's platform key (706 unless it is hardware-backed and user-authenticated); store `to_bytes()` |
| `QuickUnlock.open(app_id, account, network, KeyWrapper) -> StorageKey` | after the platform's biometric or PIN prompt; `app_id()`, `account()`, `network()`, `label()` read the record |
| `disable_quick_unlock(KeyWrapper)` | deletes the platform key; delete the stored record too |
| `validate_app_id(app_id)`, `storage_zip_number()` | the board's App identifier rules; the ZIP number to compare with `Features.storage_key` |
| `.cancel()` | from any thread; the running call ends with code 104 |

`SignResult { signed_pczt, sighash, zsig, signature_count, txid, txid_hex,
record }`: `record` is `SignResult::to_bytes`. **Persist it before
broadcasting**, broadcast idempotently, and compare after mining
([leek-zcash: Persist before broadcast](../leek-zcash/README.md#persist-before-broadcast)).

### Errors

One error type, `LeekError` (Kotlin `LeekException`, Swift `LeekError`),
by the hundreds digit of its code: `Link` (1xx), `Session` (2xx), `User`
(3xx), `Account` (4xx), `Pczt` (5xx), `Signature` (6xx), `Storage` (7xx:
an invalid app identifier, data sealed for another key 702, damaged data
703, a forgotten key 704; quick unlock: an insecure platform key 706, an
invalidated one 707, a cancelled prompt 708, a platform failure 709),
`Device` (9xx,
with the firmware's `device_code`). Each carries `code` (the stable
`leek_zcash::LeekError::code`), `recovery` (`RetrySameLink`, `Reconnect`,
`DoNotRetry`) and `reason`, a fixed sentence per code with no amounts,
addresses, keys or PCZT bytes. A malformed argument (a seed fingerprint
that is not 32 bytes) is `Pczt` 512.

### The transport

The app owns USB and Bluetooth; it implements

```kotlin
interface FrameTransport {
    fun send(bytes: ByteArray)                // throw TransportFailure.* on failure
    fun recv(timeoutMs: UInt): ByteArray      // empty: nothing arrived in time
}
```

with `LinkKind.STREAM` (USB CDC: raw bytes; framing is done in Rust, like
`leek-android`'s `UsbIo`) or `LinkKind.FRAMES` (BLE: one whole frame per
call, a big-endian u16 length with the type byte, the type, the payload,
like `BleIo`). `TransportFailure.NotFound`, `PermissionDenied` and
`Disconnected` come back as `Link` 100, 101 and 102; any other exception
thrown by the callback is 102. Reads are asked for in slices of at most
100 ms so `cancel()` is seen promptly. Device calls block: run them on a
background thread (`Dispatchers.IO`, a Swift `Task.detached` or queue).
Listeners must return quickly and must not throw.

### Quick unlock: the platform key

The app implements `KeyWrapper` over its platform's secure hardware:

```kotlin
interface KeyWrapper {
    fun info(): KeyWrapperInfo                 // hardwareBacked, userAuthRequired, label; must not throw
    fun wrap(key: ByteArray): ByteArray        // throw KeyWrapperFailure.* on failure
    fun unwrap(blob: ByteArray): ByteArray     // after the user's biometric or device PIN
    fun deleteKey()
}
```

(Swift: `protocol KeyWrapper` with `info()`, `wrap(key:)`, `unwrap(blob:)`,
`deleteKey()`.) `KeyWrapperFailure.Cancelled` comes back as 708,
`Invalidated` as 707, `Failed` and any other exception as 709. `wrap` and
`unwrap` may block while the prompt is up: call `QuickUnlock` off the main
thread. Report the flags honestly: `QuickUnlock` refuses (706) a wrapper
that does not claim both, but it cannot check the claim. Zero the
`ByteArray` / `Data` you are given and return once used.

Reference implementations, to copy and review, not built with the SDK:

| File | Platform key | Compiled here |
|---|---|---|
| `examples/quick-unlock/AndroidKeystoreWrapper.kt` | AES-256-GCM in Android Keystore (StrongBox, else TEE), per-use auth through `BiometricPrompt` with a `CryptoObject` (biometric or device credential on API 30+), invalidated by a new enrolment | compiles (kotlinc 2.0.21 against android-34, androidx.biometric 1.4.0-alpha05 and the generated bindings); not run on a device |
| `examples/quick-unlock/SecureEnclaveWrapper.swift` | ECIES to a P-256 Secure Enclave key with `.privateKeyUsage` + `.userPresence`, this device only, passcode set | no (no Swift toolchain) |
| `examples/quick-unlock/webauthn-prf.js` | a passkey's PRF output through HKDF into AES-256-GCM (Chrome wallet; there is no WebAssembly build of the SDK yet) | no |

Never implement `KeyWrapper` with a key derived from a short PIN in
software: offline, a six-digit PIN falls in seconds. Without a suitable
platform key, do not offer quick unlock
([leek-zcash: Stored app data](../leek-zcash/README.md#stored-app-data-three-tiers)).

## Host-side protection

The bindings hand the app the UFVK (`get_account`) and the account record;
they never store either. The app must:

- keep the UFVK and the encoded `AccountRecord` in the platform's secure
  storage (Android Keystore-backed encryption, the iOS / macOS Keychain);
- encrypt the wallet database as well: notes, history and memos reveal the
  user's activity on their own.

This protects a stolen device or a backup, not a malicious app; the board
cannot stop a paired host from storing the viewing key. The board asks on
its screen before releasing the viewing key, and the six-digit pairing code
is required on every connection. There is no API here that persists a UFVK
in plaintext, and none should be added.

An app can instead keep nothing readable at all: "unlock with the board",
where the database and the UFVK are sealed under the app's `StorageKey` and
the board is asked for it on every opening, or "quick unlock", where the
key is also kept wrapped by a hardware-backed key behind the user's
biometric or device PIN. The three tiers, what each protects and how to
present the choice are in
[leek-zcash: Stored app data](../leek-zcash/README.md#stored-app-data-three-tiers).
Call `forget()` when the app closes or locks: the JVM and Swift free the
object when they choose to.

## Generate the bindings

```sh
leek-zcash-ffi/scripts/generate.sh    # from host/; rewrites bindings/kotlin and bindings/swift
```

Library mode: the interface is read from the built library, so the
bindings always match the Rust. Commit the result. UniFFI checks
per-function checksums at load time, so stale bindings fail loudly.

## Build for Android (arm64-v8a, x86_64)

With the NDK and [cargo-ndk](https://github.com/bbqsrc/cargo-ndk):

```sh
rustup target add aarch64-linux-android x86_64-linux-android
cargo ndk -t arm64-v8a -t x86_64 -o <app>/src/main/jniLibs build --release -p leek-zcash-ffi
cp -r leek-zcash-ffi/bindings/kotlin/io <app>/src/main/java/
```

The app depends on `net.java.dev.jna:jna:5.14.0@aar` (5.12 or later: the
bindings use `com.sun.jna.internal.Cleaner`). 64-bit only, as the app.

## Build for iOS / macOS

```sh
rustup target add aarch64-apple-ios aarch64-apple-ios-sim aarch64-apple-darwin
for t in aarch64-apple-ios aarch64-apple-ios-sim aarch64-apple-darwin; do
    cargo build --release -p leek-zcash-ffi --target $t
done
mkdir -p headers && cp leek-zcash-ffi/bindings/swift/LeekZcashFFI.h headers/ \
    && cp leek-zcash-ffi/bindings/swift/LeekZcashFFI.modulemap headers/module.modulemap
xcodebuild -create-xcframework \
    -library target/aarch64-apple-ios/release/libleek_zcash_ffi.a -headers headers \
    -library target/aarch64-apple-ios-sim/release/libleek_zcash_ffi.a -headers headers \
    -library target/aarch64-apple-darwin/release/libleek_zcash_ffi.a -headers headers \
    -output LeekZcashFFI.xcframework
```

Add the xcframework and `bindings/swift/LeekZcash.swift` to a Swift
package target. Neither the Android nor the Apple cross-builds have been
run yet (no NDK or Xcode on the build machine); the host build and the
JVM run of the Kotlin bindings have.

## Tests

```sh
cargo test -p leek-zcash-ffi              # Rust side, zdev built with make if needed
leek-zcash-ffi/scripts/kotlin-smoke.sh    # Kotlin bindings on the JVM
```

- `tests/ffi.rs`: a Rust `FrameTransport` in both link kinds drives zdev:
  pairing (the code reaches the listener), features, account, address
  check, `sign_pczt` with progress, the same `.zsig` through `apply_zsig`
  and `finish_sd_sign`, the persisted record read back, a host-side refusal
  through the device, cancel mid-upload and from another thread, transport
  failures keeping their kind, typed byte-API errors, the account record,
  sealed app data (the draft's vector key and `db` subkey, round trips,
  damage, another key, another app, `forget`), quick unlock through a Rust
  `KeyWrapper` as a foreign one (round trip, another account, a cancelled
  prompt, a software key refused, `disable_quick_unlock`, a bad record, a
  forgotten key).
- `scripts/kotlin-smoke.sh`: compiles the committed Kotlin bindings with
  `tests/kotlin/Smoke.kt` and runs it against the freshly built library:
  the byte API, the SD request, errors by category and code, the account
  record, sealed app data, quick unlock through a Kotlin `KeyWrapper` (round
  trip, a software key refused, a cancelled prompt, disabled), a Kotlin
  transport's failures and a cancel from another thread.
  It takes the Kotlin compiler, kotlin-stdlib and JNA from the Gradle cache
  (or `KOTLIN_COMPILER_CP`, `KOTLIN_STDLIB`, `JNA_JAR`) and skips if they
  are missing; with a JNA older than 5.12 it adds a test-only `Cleaner`
  shim (`tests/kotlin/JnaCleanerShim.kt`).
- Swift: generated, not compiled here (no Swift toolchain on Linux CI yet).
