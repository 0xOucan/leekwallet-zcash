# Vendored: libzcash-ironwood-c

- Upstream: libzcash-ironwood-c (MIT, see `LICENSE`)
- Commit: `94562b8c7e5a9a3ce434445ff82481e4084e4467`
  ("v6-only: drop legacy v2-v5 wire tiers (post-NU6.3)")
- Files: copied byte for byte from upstream `src/` and `include/`; nothing
  here is edited. Only the subset the C signer needs is vendored: Pallas
  arithmetic, hash-to-curve, Sinsemilla (+ the 64 KiB S table), cmx,
  note encryption, F4Jumble/UA helpers, RedPallas, ZIP 32 Orchard keys,
  ZIP 244 v6 digests, and their support code (bignum, BLAKE2b, bech32,
  ChaCha20-Poly1305, constant-time AES for FF1, memzero).
- Not vendored: the HWP streaming protocol (`hwp*.c`, `orchard_signer.c`),
  BIP32/39, secp256k1, base58, SHA-2/HMAC/PBKDF2, the platform-less LCG
  `rand.c` (the platform supplies `random_buffer`, see `zc_platform_*.c`).

## Symbol prefix

The library ships its own copies of trezor-crypto modules under the same
global names (`bn_*`, `blake2b_*`, `bech32_*`, `chacha20poly1305_*`,
`memzero`, ...), and the firmware links trezor-crypto as well. Every
translation unit of `zsign-c` is compiled with
`-include vendor/zc_vendor_prefix.h`, which renames every global the vendored
objects define (and the RNG hooks they call) to `zcv_*`. No source file is
patched. To regenerate after re-vendoring:

```
nm -g --defined-only <vendored objects> | awk 'NF==3{print $3}' | sort -u
```

and turn each name into `#define name zcv_name` (plus `random_buffer`,
`random32`).
