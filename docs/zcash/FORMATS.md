# Wire and file formats

These are shared by the device, the CLI, the desktop and Android companions,
and the SD-card airgap. Changing one is a protocol change: bump its version
byte and keep reading the old one.

## Unsigned transaction: `*.pczt`
The PCZT bytes exactly as `pczt::Pczt::serialize()` writes them (ZIP 374:
`"PCZT"` ‖ u32 LE version ‖ postcard body). A host sends the signer view,
`zcash_client_backend::data_api::wallet::redact_pczt_for_signer(&pczt,
SignerView::Full)`. That view keeps proofs and bsk, drops witnesses, dummy
keys and wallet metadata, and stays v1-encodable where possible.

The host keeps its own unredacted copy for proving, and proves while the
device signs.

## Signatures: `*.zsig`
The device returns signatures only. The host applies them with
`Signer::apply_orchard_signature` / `apply_ironwood_signature`, which verify
each one against the action's rk before storing it.

```
offset  size  field
0       4     magic "ZSIG"
4       1     version = 1
5       32    SHA-256 of the exact .pczt bytes the device checked and signed
37      2     count N (u16 LE)
39      N*69  entries:
                1   pool   (1 = Orchard, 2 = Ironwood, 0 = transparent, reserved)
                4   action/input index (u32 LE)
                64  signature (RedPallas: R ‖ S)
```

The host refuses a `.zsig` whose hash does not match the PCZT it sent.
Signing the wrong file must fail loudly; it must not merely produce an
invalid transaction.

Pool and error codes match `components/zsign-rust/include/zsign_rust.h`
(`ZS_POOL_*`, `ZS_E_*`).

## Seed card: `*.zkv`
A seed sealed under a passphrase, for the SD storage mode (roadmap 1.4).
Written by the device (Main menu > SD Card > Save open seed) or by
`zecz zkv-new` on an offline computer. Read by the device (load as the
session's seed: nothing reaches flash) and by `zecz zkv-open`. Two independent
implementations, `firmware/src/zkv.c` and `host/zecz-cli/src/zkv.rs`, both
reproduce `vectors/zkv/golden-v1.zkv` byte for byte.

All integers little-endian.

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic `LKZV` |
| 4 | 1 | format version, 1 |
| 5 | 1 | KDF, 1 = PBKDF2-HMAC-SHA512 |
| 6 | 4 | iterations (accepted 10,000..200,000; default 20,000) |
| 10 | 16 | salt |
| 26 | 12 | nonce |
| 38 | n | AES-256-GCM ciphertext of the payload |
| 38+n | 16 | GCM tag |

- Key: the first 32 bytes of PBKDF2-HMAC-SHA512(passphrase,
  `"LeekWallet zkv v1"` ‖ salt, iterations).
- The 38-byte header is the GCM associated data, so the work factor, salt
  and nonce cannot be altered without failing the tag.
- Passphrase: 8 to 64 printable ASCII characters (0x20..0x7E). No
  normalisation is needed, so the device's text entry and a keyboard agree.

Payload, version 1:

| Size | Field |
|---|---|
| 1 | payload version, 1 |
| 1 | entropy length L: 16, 20, 24, 28 or 32 |
| L | BIP39 entropy (a 12 to 24 word phrase) |
| 4 | birthday height |
| 4 | ZIP 32 account, below 2^31 |
| 1 | flags: bit 0 `use_qsk`; others zero |
| 1 | label length, 0..32 |
| N | label, printable ASCII |

The payload holds BIP39 entropy, not the 64-byte seed. That way an SD seed
loads exactly like a typed one, and a BIP39 passphrase is applied on the
device afterwards, as for any wallet. Vault, stateless and SD modes therefore
derive the same keys from the same phrase (`firmware/sim/test_sd_seed.c`).

On the card the files sit in the root as 8.3 names, `SEED1.ZKV` to
`SEED99.ZKV`. The device never overwrites one. It writes `ZKVWRITE.TMP`,
flushes it and renames it, and removes a leftover temporary at the next mount.

## SD-card airgap layout
```
/LEEKZ/unsigned.pczt      written by the companion
/LEEKZ/unsigned.json      optional: {"network","account","created"}, display only
/LEEKZ/signed.zsig        written by the device
```
