# `leek-device`: the board's client

Website: [zcash.leekwallet.xyz](https://zcash.leekwallet.xyz) · Demo: [2-minute video](https://www.youtube.com/watch?v=8F6_8AhsnAA) · X: [@gmLeekWallet](https://x.com/gmLeekWallet) · [@0xOucan](https://x.com/0xOucan)

Our Rust client for the LeekWallet Zcash board: the v2 pairing handshake,
the encrypted session, the Zcash calls, and the `.zsig` file. It does not
care how the bytes travel, so one implementation serves
[`zecz`](../zecz-cli/README.md) and [`zecz-core`](../zecz-core/README.md)
and the Android fork (through [`leek-android`](../leek-android/README.md)).
The [Zcash LeekWallet app](../../app/README.md) speaks the same protocol
from its webview (`app/packages/core`) and does not link this crate. It is the host half of the
firmware's `session.c` and `protocol.c`; the wire format is in
[PROTOCOL.md](../../firmware/docs/PROTOCOL.md).

```
src/device.rs   pairing, session, Device and its calls, error codes
src/zsig.rs     the .zsig format
```

## Transports

`Device` talks to any `FramePort`: send one frame (type and payload),
receive one frame with a timeout.

| Link | `FramePort` | Where |
|---|---|---|
| USB serial (desktop) | `leek_transport_serial::SerialTransport`; `Device::open("/dev/ttyACM0")` | here, feature `serial` (default) |
| zdev, the simulated board | the same, on zdev's PTY | here |
| Bluetooth LE (desktop) | `BlePort` (btleplug) | the old desktop app's `link.rs` (git history; merged into `app/` on 2026-10-21) |
| USB OTG and Bluetooth LE (Android) | `JniPort`, `JniBlePort`, over Kotlin objects | [`leek-android`](../leek-android/README.md) |

Over USB every frame starts with the sync marker `LK`, then a big-endian
`u16` length, the type byte and the CBOR body; the board shares that port
with its log text. Over Bluetooth the GATT layer delimits frames, so a frame
is the same without `LK`, chunked to the link's MTU.

Use `Device::with_port(Box<dyn FramePort>)` for any other link. Without the
`serial` feature (Android) the crate does not build the desktop serial
port.

## Pairing and session

`Device::pair(show)` runs the v2 handshake:

1. `hello` with the host's X25519 key; the board answers with its key and a
   commitment to its nonce. A busy board (0x0401, waiting for its user) is
   asked again every second.
2. `helloReveal` with the host's nonce; the board reveals its nonce, and the
   commitment must open, or pairing is refused (a relay could otherwise
   choose its nonce last).
3. HKDF over the X25519 secret and the whole transcript gives two
   directional ChaCha20-Poly1305 keys and a **six-digit passkey**. `show`
   receives it; the user checks that the board shows the same digits and
   presses ALLOW there.
4. The same sealed `ping` is resent until the board answers it (until ALLOW,
   it answers in plaintext and moves no counter), or 150 s pass.

Nothing about the pairing is stored: each connection is a fresh handshake
with a passkey. After pairing, `ensure_unlocked()` asks the board for its
PIN if it is locked and waits until the user has typed it on the board; the
PIN never crosses the wire.

## Calls

| Method | Board call | What |
|---|---|---|
| `z_get_account(account, network)` | `zGetAccount` | UFVK, unified address, t-address, seed fingerprint, and the birthday if the board knows one |
| `z_show_address(account, network, ua)` | `zShowAddress` | The board proves `ua` is its own (even a rotated diversified address), shows it, and the user confirms |
| `z_set_birthday(account, network, height)` | `zSetAccountMeta` | Record the birthday on the board, one per network. True if stored in flash, false if held until the board locks |
| `z_sign(pczt, account, network, progress)` | `zSignBegin`, `zSignChunk`…, `zSignFinish` | Upload the PCZT in chunks, let the user review it, return the sighash they approved and the signatures |
| `call(method, fields, timeout)` | any | One encrypted request and its reply |

`network` is `"test"` or `"main"`. Results are CBOR maps; `get_text`,
`get_bytes`, `get_uint` read their fields. Calls that derive keys wait up to
90 s, calls that wait for the user up to 150 s. `ZECZ_DEBUG=1` prints each
request and reply frame.

### Errors

A board error is a `DeviceError { code, message }`; `is_code(&err, code)`
tests for one, also under added context.

| Code | Constant | Meaning |
|---|---|---|
| 0x0100 | `ERR_NOT_UNLOCKED` | The board is locked |
| 0x0200 / 0x0201 | `ERR_USER_REJECTED` / `ERR_USER_TIMEOUT` | The user said no, or did not answer |
| 0x0300 | `ERR_NO_WALLET` | No seed to derive from (no wallet, derivation failed, or the wallet still opening) |
| 0x0400 / 0x0401 | `ERR_SESSION` / `ERR_BUSY` | Session error; the board is waiting for its user |
| 0x05xx | `ERR_ZSIGN_BASE` | The board refuses to sign this PCZT; the low byte is the reason ([PROTOCOL.md](../../firmware/docs/PROTOCOL.md)) |
| 0x0528 | `ERR_BOARD_NETWORK` | The request names the other network than the board's own setting |

**Network refusal (0x0528).** The board answers only for its own network
(Settings > Network) and refuses the other before deriving anything. `call`
adds a sentence for the user, from `wrong_network_message`: "The board is
set to Testnet, but this app is on Mainnet. Switch the board to Mainnet in
Settings → Network, or use the Testnet app." The code stays underneath, so
`is_code(&e, ERR_BOARD_NETWORK)` still works.

**Retry on 0x0300.** Right after connecting (over Bluetooth especially, and
right after the PIN) a board that holds a wallet could answer 0x0300: its
seed was not in RAM yet. So the first `zGetAccount` of a session that gets
0x0300 is asked once more after 1.5 s (`NO_WALLET_RETRY`). After one
answer, a 0x0300 is the board's word and is not retried.

`z_show_address` turns its refusals into plain sentences too: an address
that is not the board's ("this app may be compromised"), an address with a
receiver the board cannot check (Sapling), or the user saying the addresses
differ.

## `.zsig`

The board returns signatures only. `Zsig::new(pczt_bytes, entries)` binds
them to the SHA-256 of the exact bytes the board reviewed; `to_bytes`,
`parse`, and `parse_for(bytes, pczt_bytes)` (parse and check the hash).

```
offset  size  field
0       4     magic "ZSIG"
4       1     version: 1 if every entry is shielded, 2 if any is transparent
5       32    SHA-256 of the exact .pczt bytes the board checked and signed
37      2     count N (u16 LE)
39      ...   entries: pool (1) | action/input index (u32 LE) | then
                pool 1 (Orchard), 2 (Ironwood): signature (64, RedPallas R || S)
                pool 0 (transparent): L (1) | DER signature || sighash type (L) | pubkey (33)
```

A shielded-only file is always version 1. The full description is in
[FORMATS.md](../../docs/zcash/FORMATS.md).

## Tests

```sh
cd host && cargo test -p leek-device
```

The unit tests cover the error wording, the 0x0300 retry (only the first
account request, only on 0x0300) against a scripted device, and the `.zsig`
format. The whole path against the firmware's own code runs in
`zecz-core`'s `tests/device_zdev.rs`.
