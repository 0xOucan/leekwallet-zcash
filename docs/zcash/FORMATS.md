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
each one against the action's rk before storing it, and, for a transparent
input, `Signer::append_transparent_signature`, which verifies it against the
input's own sighash and the pubkey the input's script pays to.

```
offset  size  field
0       4     magic "ZSIG"
4       1     version: 1 if every entry is shielded, 2 if any is transparent
5       32    SHA-256 of the exact .pczt bytes the device checked and signed
37      2     count N (u16 LE)
39      ...   N entries, each:
                1   pool   (0 = transparent, 1 = Orchard, 2 = Ironwood)
                4   action/input index (u32 LE)
              pool 1, 2 (69 bytes in all):
                64  signature (RedPallas: R ‖ S)
              pool 0 (version 2 only; 1 + 4 + 1 + L + 33 bytes):
                1   L, 9..73
                L   DER ECDSA signature (secp256k1, low S) ‖ sighash type
                33  compressed public key it verifies under
```

A version 1 file is byte for byte what it always was, so a shielded-only
`.zsig` still reads in a version 1 reader. A version 2 reader reads both.
In version 1 a pool 0 entry is an error; in version 2 the pool decides the
entry's length. The sighash type is always `SIGHASH_ALL` (0x01): the device
refuses any input that asks for another one.

The pubkey lets a host build the P2PKH scriptSig (and the pczt crate's
`hash160_preimages` entry that `append_transparent_signature` needs) without
trusting anything but the signature check: the host must still verify the
signature against its own sighash, which the pczt Signer does.

The host refuses a `.zsig` whose hash does not match the PCZT it sent.
Signing the wrong file must fail loudly; it must not merely produce an
invalid transaction.

Pool and error codes match `components/zsign-rust/include/zsign_rust.h`
(`ZS_POOL_*`, `ZS_E_*`).

### Transparent inputs (roadmap 10.1)
The device signs P2PKH inputs of its own account only. For each transparent
input of a PCZT, both signer backends check, in this order, and refuse the
whole PCZT otherwise:

1. The input parses as the pczt crate parses it (value at most 21M ZEC,
   scripts well formed, a known sighash type, lock-time ranges): else
   `ZS_E_PARSE`.
2. `sighash_type` is `SIGHASH_ALL`; `script_pubkey` is exactly P2PKH
   (`76 a9 14 <20> 88 ac`); there is no `redeem_script`, `script_sig`,
   partial signature or required lock time: else `ZS_E_UNSUPPORTED_BUNDLE`.
3. `bip32_derivation` has an entry: else `ZS_E_MISSING_DATA`. More than one
   is `ZS_E_UNSUPPORTED_BUNDLE`.
4. That entry is ours: the ZIP 32 seed fingerprint of this seed, the path
   `m/44'/coin'/account'/scope/index` with this network's coin type and the
   account being signed for, scope 0 (external), 1 (internal) or 2
   (ephemeral), index non-hardened; the pubkey the device derives at that
   path is the entry's key; and HASH160 of that key is the hash in
   `script_pubkey`. Any failure is `ZS_E_SPEND_NOT_OURS`.

The review then lists every input as "from your t-address" with its value
and address, and the fee counts transparent inputs:
fee = shielded spends + transparent inputs − shielded outputs − transparent
outputs (`ZS_E_VALUE_BALANCE` if negative). The input values cannot be
misstated to the device: ZIP 244 commits every input's amount and
script_pubkey into each transparent sighash and into the shielded sighash,
so a PCZT that lies about them yields signatures the network rejects.

Each input is signed over its ZIP 244 transparent signature digest (S.2,
`SIGHASH_ALL`, v6 digest tree) with the key at its path: secp256k1 ECDSA,
RFC 6979 nonces, low S. Both backends derive the same RFC 6979 nonce, so
their signatures are byte-identical (`test_zsign_diff` checks it).

Memo recovery also tries the account's transparent outgoing viewing keys
(ZIP 316: `PRF^expand(chain code, [0xd0] ‖ account pubkey)`, internal then
external half), which is what a wallet uses for outputs funded only by
transparent inputs.

## Seed card: `*.zkv`
A seed sealed under a passphrase, for the SD storage mode (roadmap 1.4).
Written by the device (Main menu > SD Card > Save open seed) or by
`zecz zkv-new` on an offline computer. Read by the device (load as the
session's seed: nothing reaches flash) and by `zecz zkv-open`. Two independent
implementations, `firmware/src/zkv.c` and `host/zecz-core/src/zkv.rs`, both
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
