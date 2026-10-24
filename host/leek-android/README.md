# `leek-android`: the native library of the Android app

Website: [zcash.leekwallet.xyz](https://zcash.leekwallet.xyz) · X: [@gmLeekWallet](https://x.com/gmLeekWallet) · [@0xOucan](https://x.com/0xOucan)

Our JNI library (`libleek_android.so`) for
[LeekWallet Zcash for Android](https://github.com/0xOucan/leekwallet-zcash-android),
our fork of Zodl (branch `leekwallet`). It gives the app two things the Zodl
SDK does not have:

- **The board's client.** Android only lets the Kotlin side open a USB
  device or a Bluetooth link, so Kotlin moves the bytes and everything above
  that (framing, the v2 pairing, the encrypted session, the `z*` calls) is
  [`leek-device`](../leek-device/README.md), the same Rust that `zecz` uses.
  Nothing of the protocol is rewritten in Kotlin.
- **The signature step.** The board returns a `.zsig` (signatures only).
  The Zodl SDK can combine a *signed* PCZT with the proven one
  (`createTransactionFromPczt`), as it does for Keystone and Ledger, but it
  has no call to insert raw signatures. This library does it with the `pczt`
  crate's Signer role, which checks each signature against its key before
  storing it, exactly as `zecz finish` does on a PC. It also makes the
  compact signer view the app sends to the board.

```
src/lib.rs      compact_view, apply_signatures(_to), the PcztSignatures JNI functions
src/device.rs   JniPort (USB), JniBlePort (Bluetooth), the LeekDeviceNative JNI functions
tests/apply.rs  the signature step against the vectors
```

## JNI functions

Every call blocks: run it off the main thread. Errors are thrown as Java
exceptions with the reason (`IllegalStateException` for device calls,
`IllegalArgumentException` for the PCZT functions).

**`io.leekwallet.zcash.hw.LeekDeviceNative`**

| Function | What |
|---|---|
| `open(io: UsbIo): Long` | A device over USB OTG. `UsbIo` has `write(ByteArray)` and `read(timeoutMs: Int): ByteArray?`; the library does the USB framing (`LK` marker) |
| `openBle(io: BleIo): Long` | A device over Bluetooth LE. `BleIo` has `sendFrame(ByteArray)` and `recvFrame(timeoutMs: Int): ByteArray?`, whole frames (`u16` length, type, payload); Kotlin does the GATT chunking |
| `close(handle)` | Free the handle |
| `pair(handle, listener)` | The v2 handshake; `listener.onPasskey(String)` gets the six digits to compare with the board, and the call returns once the user pressed ALLOW there |
| `ensureUnlocked(handle)` | Ask for the PIN on the board if it is locked, and wait |
| `getAccount(handle, account, network): String` | JSON: `account`, `network`, `ufvk`, `ua`, `tAddress`, `seedFingerprint` (hex), and `birthday` when the board knows it. A first 0x0300 is retried once |
| `setBirthday(handle, account, network, birthday): Boolean` | Record the birthday on the board; true if stored in flash, false if only until it locks |
| `showAddress(handle, account, network, ua)` | The board checks that `ua` is its own and shows it for the user to confirm |
| `sign(handle, pczt, account, network): ByteArray` | Upload the PCZT, let the user review it on the board, return the `.zsig` |

`network` is `"main"` or `"test"`: each of the two apps sends its own, and
the board refuses the other network than its setting with error 0x0528
(the message tells the user to switch the board or use the other app).

**`io.leekwallet.zcash.hw.PcztSignatures`**

| Function | What |
|---|---|
| `compact(pczt): ByteArray` | The compact signer view of a PCZT (v2 encoding, about half the bytes): what the app sends to the board |
| `apply(pczt, zsig): ByteArray` | Apply a `.zsig` made for exactly these PCZT bytes; return the signed PCZT |
| `applyTo(pczt, sent, zsig): ByteArray` | Apply a `.zsig` made for `sent` (the compact view, or `pczt` itself) to the full `pczt`. Refused unless `sent` is what the board signed and has the same sighash as `pczt` |

Shielded signatures (Orchard, Ironwood) are checked against each action's
key; transparent ones (a version 2 `.zsig`, from a shield) against each
input's sighash and the key its script pays to.

## Build

The library is built by the Android fork, from its checkout at `android/`
in this repository (ignored by git here):

```sh
git clone -b leekwallet https://github.com/0xOucan/leekwallet-zcash-android android
rustup target add aarch64-linux-android
android/tools/leekwallet/build-native.sh
```

`build-native.sh` runs
`cargo build --release -p leek-android --lib --target aarch64-linux-android`
in `host/` with the NDK's clang as the linker (`ANDROID_NDK_HOME`, default
`~/Android/Sdk/ndk/27.2.12479018`, API level 26), and copies
`libleek_android.so` to `feature-leekwallet/src/main/jniLibs/arm64-v8a/`,
where Gradle packages it. Only arm64-v8a is built.

The crate turns off `leek-device`'s `serial` feature: on Android the
transport is the Kotlin object, not a serial port.

## Tests

```sh
cd host && cargo test -p leek-android
```

`tests/apply.rs` signs the PCZT vectors with the soft signer and checks that
the signatures apply, prove and extract into a transaction that verifies,
for full and compact views and for transparent inputs; and that a `.zsig`
for other bytes, a bad signature, a compact view of another transaction or
signatures over a changed memo are refused.
