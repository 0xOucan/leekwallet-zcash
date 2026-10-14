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

## SD-card airgap layout
```
/LEEKZ/unsigned.pczt      written by the companion
/LEEKZ/unsigned.json      optional: {"network","account","created"}, display only
/LEEKZ/signed.zsig        written by the device
```
