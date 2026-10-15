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

### 1.1 Remove Ethereum — done
Branch `phase1-strip-eth`.
- **Removed:** `eth-decode`, `eth-tx`, `eth-tx-rlp`, `eip712`, `eip4527` and
  `eip4527-encode`, along with their sim suites, asan/conformance targets
  and mutants.
  - Protocol: `getAddress`, `signTransaction`, `signMessage` and
    `signTypedData`, plus the air gap sign path.
  - UI: the ETH review pages, the result screen, the address browser and
    QR, and the crypto-hdkey export.
  - Vault: the ETH address, the `wallet_sign_*` calls and the m/44'/60'
    account key.
  - Build: `-DUSE_ETHEREUM`/`-DUSE_KECCAK` in platformio.ini and the sim.
    trezor-crypto still forces both in its own options.h and CMakeLists.
- **Kept:** vault, PIN, passphrase, temp seed, BIP39/BIP32/secp256k1, the
  generic protocol commands, UR/QR/camera/BLE, and the blind-signing module.
  - getFeatures now reports `chain: "zcash"`.
  - setPassphrase confirms and returns the seed fingerprint.
  - Wallet Info and Show Address say "Zcash: coming soon".
  - Scan assembles URs and refuses every type until zcash-pczt (Phase 5).
- **Tests:** `make -C sim test` passes all 29 suites, and asan and fuzz are
  clean.
- **Builds:** esp32s3, esp32s3cam and esp32s3-qemu build.

| esp32s3 | Before (main 50a454d) | After | Change |
|---|---|---|---|
| RAM | 74,120 B | 67,120 B | −7,000 B |
| Flash | 1,160,429 B | 1,118,397 B | −42,032 B |

- **Not done:** `app/` still speaks the ETH protocol and no longer matches
  the firmware. Board boot is pending (no board on hand).

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

### 4.1, 4.2, 4.4 C signer (`components/zsign-c`): done; 4.3 with Phase 3; 4.5 QEMU only
`zsign-c` implements the same `new / check / sign / verify_sig / free` ABI as
zsign-rust (shared types in `components/zsign-common`) and shares no code with
it. It uses a vendored, unedited subset of libzcash-ironwood-c (94562b8, all
globals renamed to `zcv_*` by a forced-include header) and a bounded postcard
PCZT reader of our own.

- 4.2: `test_zcash_pczt_c` passes all 14 PCZT vectors (5 valid, 9 refusals,
  exact codes) and 48/48 key vectors.
- 4.4: `test_zsign_diff` runs both backends over every vector and 3,675
  single-byte mutations: 2,133 accepted by both, 0 verdict, summary
  (sighash included) or refusal-code mismatches. Every 8th accepted mutation
  is signed, and each backend's signature verifies under the other. A
  deterministic-nonce mode was not added; cross-verification replaces it.
  `make asan` runs the diff test with the C library under ASan + UBSan: clean.
- The diff work hardened the Rust signer too: it now refuses trailing bytes
  after the postcard body and any tx version other than v6.
- 4.3 (`BOTH` mode) needs the sign protocol, so it lands with Phase 3.

QEMU, `iw_send_shielded` (4,807 B), each backend in a fresh 64 KiB task:

| Measure | Rust | C |
|---|---|---|
| Summary / sighash / fee | MATCH | MATCH (summaries byte-equal) |
| Stack used | 50,088 B | 5,056 B |
| Heap used | ≈ 28 KB | ≈ 7 KB |
| QEMU parse / check / sign / verify | 8 ms / 1.45 s / 0.83 s / 0.25 s | 2 ms / 0.36 s / 0.05 s / 0.09 s |
| One-time self-test | n/a | 0.10 s |

QEMU times are not silicon times; 4.5 needs the board. Both firmware targets
build with the component, and it compiles for the Pixie's RISC-V as well.

Open: `test_protocol_asan` reports UBSan misaligned u32 loads in the inherited
trezor-crypto `chacha_merged.c` (32 reports, not new; harmless on x86, to be
checked on Xtensa).

### 3.1–3.5 and 4.3 Device signing path: done on PC; 3.6 pending funds and board
- **3.1 intake** (`src/zcash-sign.c`): `zSignBegin/zSignChunk/zSignFinish`,
  ordered chunks of up to 3072 B into PSRAM (internal RAM where there is
  none), 512 KB cap, SHA-256 checked before any parser runs.
- **4.3 BOTH mode** (default): Rust and C check the same bytes; summaries
  must be byte-identical; Rust signs; each signature is verified by both.
  An injected disagreement is refused (`test_zcash_sign`).
- **3.4 review** (`src/zcash-review.c`, `SCREEN_ZSIGN_REVIEW`): recipient
  pages with full address and memo, change, fee/network/upgrade, final
  question; ACCEPT signs only on the last page (mutation-checked).
- **3.5 signatures**: `{sighash, signatures:[{pool,index,sig}]}`; refusals
  are `0x05xx` with the reason in the low byte (PROTOCOL.md).
- **Companion**: `zecz device-init` and `zecz sign-device` (`src/device.rs`,
  the v2 session in Rust).
- **No board needed for any of the above**: `firmware/sim/zdev` runs the
  firmware's protocol, session, both signers and review pages on a PTY.
  `tests/device_zdev.rs`: the device's signatures apply, prove and extract
  into a verifying transaction; tamper and rejection give nothing.
  `device-init` through zdev against testnet imported the same account as
  the dev path.
- Firmware: esp32s3 and pixie build; the esp32s3-qemu image boots to
  "Initialization complete" with the worker running.
- Found and fixed: PlatformIO drops `-include` from IDF compile flags, so
  zsign-c was built without its symbol prefix (would have bound to
  trezor-crypto's bignum/BLAKE2b once linked). Wrapper units in
  `components/zsign-c/idf/`; `scripts/check-zsign-c-symbols.sh` verifies.
- Closed: the UBSan ChaCha alignment reports are x86-only; on Xtensa and
  RISC-V trezor-crypto's ecrypt config takes the byte-wise path.
- Open: 3.6 (a confirmed testnet send) needs testnet funds, and then the
  board; real timings for check/sign and the review on the panel need the
  board too.
