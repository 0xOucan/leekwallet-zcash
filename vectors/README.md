# Test vectors

Known-answer data shared by every implementation: the firmware's two
signers (C and Rust), the simulator suites, the host crates and the Android
library. Generated from librustzcash, so each side is checked against the
reference, not against itself.

| Path | What |
|---|---|
| `keys.json`, `keys_vectors.h` | Accounts derived from public test mnemonics (e.g. "abandon … art"), mainnet and testnet, with and without a passphrase: seed fingerprint, Orchard FVK and address, transparent account key and address, UFVK, UA |
| `pczt.json`, `pczt_vectors.h`, `pczt/*.pczt` | 40 PCZTs: valid sends (Ironwood, Orchard→Ironwood, NU7, mainnet, transparent shields and mixed, compact views) with their expected summaries and sighashes, and `r_*` refusals each with the error code the device must return |
| `zsig/` | A `.zsig` for `iw_send_shielded.pczt` |
| `zkv/` | A golden `.zkv` SD seed card (format v1) |
| `gen/` | The generators |

Never edit these files by hand. The generators use a seeded RNG, and new
vectors are only appended: existing files must stay byte-identical, since
other files (a `.zsig`, firmware tests) pin their hashes.

```sh
cd gen
cargo run --release                        # keys.json, keys_vectors.h
cargo run --release --bin pczt_vectors     # pczt.json, pczt_vectors.h, pczt/*.pczt
git diff --stat ..                         # only additions expected
```

The rule from the roadmap: nothing reaches the signing path without an
`r_*` vector proving it refuses the bad case.
