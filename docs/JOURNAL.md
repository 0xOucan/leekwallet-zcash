# Journal

## Phase 0: Foundations

### 0.1 Repository: done
`leekwallet-zcash/` created. LeekWallet `main` (6d1abb1, 656 commits) is
imported with its history under `firmware/` via `git subtree`. The docs moved
here from `referencialeekwallet/zec hardware wallet/`.

### 0.2 Baseline: done (host side), board pending
- `pio run -e esp32s3` on unmodified LeekWallet: SUCCESS, RAM 22.6% (74,120 B),
  flash 27.7% (1,160,429 B of the 4 MB app partition).
- `make -C sim test`: 27 suites PASSED, 0 failures.
- Board flash and boot: pending (no board on hand).

### 0.3 Toolchains: done
See `TOOLCHAIN.md`.

### 0.4 Rust-on-Xtensa spike: PASS in QEMU, board timings pending
`components/zsign-rust` is a no_std staticlib built on upstream `orchard` 0.16
(default-features off, so no Halo2 or std), `pasta_curves` 0.6.1 and `zip32`
0.3. `spike/zspike` is a minimal IDF app that runs it; `run-qemu.sh` drives
the whole check.

- Xtensa compile: OK with Rust esp 1.99.0.0. The ICE from esp-rs/rust#277 did
  not reproduce for this crate set. The one blocker was radium (fixed above).
- Link: the spike app is 513,643 B of flash including ESP-IDF.
- Under QEMU (esp32s3 machine, 240 MHz config):

| Operation | QEMU µs (not real time) |
|---|---|
| ZIP 32 sk → FVK (includes commit_ivk/Sinsemilla) | 740,533 |
| Default address (FF1 + Sinsemilla) | 645,554 |
| RedPallas SpendAuth sign (rsk = ask + α) | 645,278 |
| NoteCommit → cmx | 712,348 |

  Heap peak is about 41 KB (366,404 → 325,380 B min free), with no PSRAM.
  Stack used is about 8.6 KB of 32 KB.
  QEMU's TCG timings do **not** reflect silicon; real numbers come from the
  board (same app, `pio run -e esp32s3 -t upload`).
- Cross-architecture check (`host/host_check.c`):

  | Check | Result |
  |---|---|
  | FVK | MATCH |
  | Address | MATCH |
  | cmx | MATCH |
  | Device signature verified on host against rk = ak + [α]G | VALID |
  | Flipped-message negative control | REJECTED |
  | Overall | **RESULT: PASS** |

- Decision: the Rust backend is viable on Xtensa, so Phase 1 proceeds with
  Rust as the primary signer. The C backend still comes in Phase 4.

### 0.6 Test vectors (keys + PCZT): done
- `vectors/gen` (Rust, librustzcash at 16f0723) emits 48 key vectors
  (mainnet + testnet, 4 mnemonics, 3 passphrases, 2 accounts).
  `test_zcash_keys` checks trezor-crypto (seed, BIP44, t-address, P2PKH
  receiver) and zsign-rust (seed fingerprint, Orchard FVK and receiver, UFVK,
  UA) against them: 48/48 pass, and a mutation check fails as it should.
- `pczt-vectors` (deterministic, seeded ChaCha20) builds testnet NU6.3 v6
  PCZTs the way a wallet prepares them for a hardware signer (Builder →
  Creator → IO Finalizer → Updater with ZIP 32 paths and user_address).
  There are 2 valid sends (shielded with memo; to a tm address) and 7
  refusals (foreign spend, tampered value, tampered recipient, tampered
  user_address, truncated, bad magic, NU6.2 branch id).

### 1.3 Account derivation (Rust): done
`zs_account_derive`: ZIP 32 seed fingerprint, Orchard FVK and receiver, and
the Orchard + P2PKH UFVK and UA. The transparent half is supplied by
trezor-crypto, so each export joins two implementations.

### 3.1–3.5 Rust signer (host + QEMU): done, board pending
`zs_pczt_new / check / sign / verify_sig` (C ABI, `src/signer.rs`), using
upstream `pczt` 0.10.0-pre.2 (signer feature) and `orchard` 0.16:
- **Refuses:** branches other than NU6.3/NU7; coin type ≠ network;
  transparent inputs and Sapling (for now); `dummy_sk`; pre-signed spends
  with value.
- **Spends:** `verify_nullifier` + `verify_rk` against our own FVK.
- **Outputs:** `verify_note_commitment` on every output; `cv_net` when rcv
  is present; `user_address` must decode on this network and contain the
  committed receiver; memo recovered with our own OVK (ZIP 302
  classification).
- **Bundle:** Σ(spend − output) must equal `value_sum`; the fee is
  computed on the device.
- **Signing:** only after a passing check, once per check, with the same
  seed. Signatures are returned as {pool, action index, sig} so the host
  applies them (signatures-only output keeps the C backend free of a PCZT
  writer).

`test_zcash_pczt` (host): 9/9 vectors. The sighash matches librustzcash,
the fee and outputs match, signatures verify against rk, and every
refusal carries the exact expected code. Mutation check OK.

On Xtensa under QEMU (`spike/zspike/run-qemu.sh`), with the
iw_send_shielded PCZT of 4,807 B:

| Measure | Value |
|---|---|
| check / sign | OK |
| Sighash | MATCH |
| Fee | MATCH |
| Signature | verifies |
| Heap peak | ≈ 28 KB (no PSRAM) |
| **Stack peak** | **≈ 50 KB** (64 KB task; 15,588 B left) → the signing task needs a 64 KB stack |
| Firmware | 876,144 B |
| QEMU times (not silicon) | parse 15 ms, check 2.4 s, sign 1.4 s |

Toolchain finding: libsecp256k1 had been built big-endian (see
TOOLCHAIN.md).

### 2. Host CLI (`host/zecz-cli`, testnet): wallet + PCZT flow done, live send pending funds
`zecz` is a watch-only wallet on `zcash_client_backend` / `zcash_client_sqlite`
(path deps on the same librustzcash checkout). Commands: `init` (device UFVK,
records the ZIP 32 seed fingerprint and account so PCZT spends carry
`zip32_derivation`), `init-dev` and `sign-soft` (DEV ONLY, mnemonic stand-ins
for the device), `tip`, `sync`, `balance`, `address`, `send` (full PCZT kept in
`pending/<sha256>.pczt`, `SignerView::Full` written for the device), and
`finish` (hash-checked `.zsig` → `apply_*_signature` → prove → store →
`SendTransaction`).

- Tests: 8 `.zsig` unit tests; 5 integration tests on the committed vectors
  (dev keys = keys.json; soft sign → `.zsig` → `apply_ironwood_signature`
  verifies; corrupted signature and wrong-PCZT hash refused; full sign →
  prove → extract of `iw_send_shielded` verifies).
- Live, testnet.zec.rocks (tip 4,502,908, branch NU7 `77190ad9`): `init-dev`
  with abandon…art at birthday 4,501,900 reproduces the vector UFVK and UA;
  `sync` of ~1,000 blocks takes under 3 s; balance 0. `send` then fails with
  "Insufficient balance", as it should. A funded end-to-end send is next.
- Transparent funds show in `balance` but cannot be sent yet (the device
  refuses transparent inputs).
