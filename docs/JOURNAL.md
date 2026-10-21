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

### 1.5 and 1.6 Seed fingerprint and receive screens: done on PC; phone scan needs the board
- **1.5 fingerprint**: the home screen header reads `ZFP 1A2B3C4D` right
  after unlock (and after a passphrase, wallet switch or temporary seed),
  the wallet and passphrase-confirm screens show it beside the BIP32 XFP.
  It is the first four bytes of the ZIP 32 seed fingerprint
  (`zcash_seed_fingerprint()`, trezor-crypto BLAKE2b; matches librustzcash
  on all 48 key vectors, passphrases included). `zecz` prints the same
  `ZFP` on `init`/`device-init`. The three test passphrases give three
  different short forms. Every lock path clears it (mutation-checked).
- **1.6 receive** (`src/zcash-receive.c`, Main menu > Receive): the UA
  whole over 21-column pages, its QR, the t-address marked PUBLIC, its QR;
  UP/DN page, OK flips text and QR, BACK leaves. Settings > Network picks
  mainnet or testnet (persisted; a view only, signing follows the PCZT).
- QR codes carry the address lowercase in byte mode: zcash_address takes
  the HRP as written, so an uppercase `UTEST1...` would scan but not parse.
  A UA is v7 at scale 1 (45 px), a t-address v3 at scale 2. Every vector's
  UA and t-address QR is drawn into the real framebuffer and scanned back
  with `zbarimg` in `test_zcash_receive` (96 scans, all exact).
- Open: whether a phone reads the scale-1 UA QR off the real 128x64 panel
  (the roadmap's own question) needs the board; the text pages are the
  fallback either way.

### 1.4 SD card storage mode: done on PC; the card itself needs the board
- **Format** (`.zkv` v1, FORMATS.md): `LKZV` header with the KDF parameters
  as GCM associated data, then AES-256-GCM over BIP39 entropy, birthday,
  account, flags and label. PBKDF2-HMAC-SHA512, default 20,000 iterations
  (about 3 s on the S3 at 240 MHz, estimated from the vault's measured
  0.226 ms/iteration at 160 MHz), refused outside 10,000 to 200,000.
- **Two implementations**: `firmware/src/zkv.c` (trezor-crypto) and
  `host/zecz-cli/src/zkv.rs` (RustCrypto), written separately. Both produce
  `vectors/zkv/golden-v1.zkv` byte for byte and refuse every single-bit flip
  of it (840 flips, 105 truncations in C).
- **Device**: Main menu > SD Card lists `SEEDn.ZKV`, loads one as the session
  seed (passphrase on the four buttons; the home header then reads
  `TEMP  ZFP ...`), or saves the open seed under a passphrase entered twice.
  It never overwrites, writes through a temporary plus rename, and reads the
  file back before saying it saved. `sdcard.c` is the same POSIX code on
  FATFS and on a host directory, so the sim's fake SD is the real code.
- **Done-when checks** (`test_sd_seed`, real vault and real derivation):
  vault, stateless and SD give the same UFVK, UA and fingerprint for all 48
  vectors. A random phrase loaded from the card leaves neither the phrase
  nor 8 bytes of its entropy anywhere in flash. A wrong passphrase loads
  nothing and leaves the open wallet as it was. A write cut before the
  rename leaves no seed file and is cleaned up at the next mount. The
  golden file loads on the device and derives the vector's keys.
- **Boards**: SD on the S3 reference profile too (GPIO 38/39/40, 1-bit
  SDMMC, the CAM board's wiring; it needs a passive microSD breakout, not a
  level-shifted SPI module). esp32s3, esp32s3cam and pixie build; QEMU boots.
- `zecz zkv-new --mnemonic ... --out SEED1.ZKV --birthday H` and
  `zecz zkv-open --in FILE` (shows ZFP, account, birthday, UFVK; never the
  phrase). The file passphrase comes from `ZECZ_ZKV_PASSPHRASE` or stdin.
- Open: per-account metadata in the vault (birthday, `use_qsk`, label);
  mounting a real card, and the KDF time on silicon.
- **Per-account metadata** (`src/zcash-meta.c`): birthday, flags
  (`use_qsk`) and label, keyed by the short seed fingerprint and the
  account. In NVS for a stored wallet with no passphrase. In RAM until lock
  for a passphrase, temporary or SD seed, so flash never records that a
  hidden or session seed was used (`test_zcash_meta` searches flash for the
  fingerprint and the birthday). A power cut at any write leaves the old or
  the new record, and a damaged record reads as none.
- `zGetAccount` now returns `birthday` when known. The new `zSetAccountMeta`
  records it. `zecz device-init --birthday` is optional when the device
  knows it, and an import records it on the device. An SD load records the
  file's birthday, and an SD save writes the known one into the file.
- The fingerprint helpers moved to `src/zcash-fp.c`.

### 1.7 zGetFeatures and zShowAddress: done on PC
- `zGetFeatures` (pre-session) gives the backend mode, the accepted branch
  IDs, v6 only, Orchard spends only, the PCZT and chunk limits, and the
  storage modes. `ZCASH_SIGN_BRANCHES` is checked in `test_zcash_sign`: every
  branch a backend accepted is advertised, and every advertised one was
  accepted by both.
- `zShowAddress`: the device derives the address and pages the whole UA on
  `SCREEN_ADDRESS_CHECK`. YES is only on the last page, NO works from any
  page, and an expired request takes the question down. The address is
  returned only after a yes.
- `zecz device-show-address --port P` runs it, and the zdev tests cover a
  yes, a no, and an address that differs from the expected one.
- Phase 1 is now done on PC. Waiting on the board: panel and buttons, the
  SD card, the KDF and derivation times, and the phone-scan check for the
  UA QR.

### S3 CAM screen kit: 128x128 SH1107, EC11 encoder, four-button module
- Wiring in `firmware/docs/WIRING-S3CAM.md`:
  - OLED on I2C (GPIO47 SDA, GPIO21 SCL).
  - Buttons on GPIO1, 41, 14 and 42.
  - Encoder CLK on GPIO44 and DT on GPIO43 through 1 kΩ. Its switch is in
    parallel with OK (GPIO42).
  - Everything on 3V3.
- `src/panel128.c`: the 128x64 UI frame centred on the 128x128 panel. The
  last QR drawn (remembered by oled-core) is redrawn full-panel: UA at
  2 px per module, t-address at 3. `test_panel128` scans 32 of them back
  with zbarimg.
- `src/encoder.c`: full-step quadrature state machine, polled every 1 ms in
  the button task. `test_encoder` covers a detent each way, bounce, a
  half-turn back, noise and random input.
- `oled.c` has an SH1107 path (Adafruit's 128x128 init values; page mode).
  `BOARD_SH1107_FLIP`, `BOARD_ENC_REVERSE` and `ENCODER_HALF_STEP` are the
  bring-up switches. esp32s3cam, esp32s3 and pixie build.
- Testnet: a private test seed was generated into `~/zecz-test/testnet-seed.txt`
  (mode 600, outside the repo). Imported via zdev into `~/zecz-test/wallet`
  with birthday 4,506,500 and ZFP 57F3A27A.

### 3.6 First real transaction: signed on the board, broadcast on testnet (2026-10-15)
- Board: ESP32-S3-N16R8 CAM with the screen kit (SH1107 128x128, EC11,
  OV5640), firmware `esp32s3cam`, C signer backend.
- Wallet created on the device (ZFP 551A1FD4). `zecz device-init` imported
  it with birthday 4,506,700, and `device-show-address` was confirmed on
  the device.
- A faucet sent 0.125 TAZ (txid aba1ae7b…2ce18995), received into the
  Ironwood pool and spendable after 10 confirmations (ZIP 315 default for
  untrusted notes).
- `zecz send` made 0.01 TAZ to self with a memo: a 2,791-byte signer-view
  PCZT. `sign-device` sent `zSignBegin`, one `zSignChunk` and
  `zSignFinish`. The device reviewed the transaction and signed one
  Ironwood action. `finish` applied the signature (verified against rk),
  proved, and broadcast **txid
  1daaff6366fccbd6ea50f137a975a5013b2632083446b5220205ad16057b1ef0**.
- Bugs this bring-up found, all fixed on main:
  - The USB driver silently dropped writes over 256 bytes.
  - Rust derivation took 97 s on the chip (C: 0.75 s).
  - The vault opens about 1.5 s after the PIN is accepted.
  - Logs could interleave with frames.
  - `zecz`'s 10 s timeout was too short for derivation.
  - The SH1107 needed an address probe and bus scan.
- Diagnostic used: `~/zecz-test/diag/serial_tap.py`, a PTY relay that
  records the board's log while zecz talks through it.

### 1.4 SD card storage mode on the board (2026-10-15)
- The 3.7 GB FAT32 card in the S3 CAM board's slot worked: *SD Card → Save
  open seed* wrote `SEED1.ZKV`. It is 98 bytes: a 24-word seed with no
  label, birthday 4,506,700 taken from the account metadata zecz recorded,
  and 20,000 PBKDF2 iterations. The write went through a temporary file
  plus rename and left no temporary behind.
- On the PC, `zecz zkv-open` (the Rust implementation) opened the
  board-written file and showed ZFP 551A1FD4, matching the device.
- On the board, a wrong passphrase was refused. The right one loaded the
  seed as a session seed (`TEMP  ZFP 551A1FD4`), and a lock dropped it.
- The file timestamp reads 1979 because the board has no clock. Harmless.

### 5 SD card airgap: a testnet send signed from the card (2026-10-15)
- `zecz send --out /media/…/LEEKZ/unsigned.pczt` wrote 0.005 TAZ to self
  with a memo. On the board, *SD Card → Sign transaction* ran the same
  check and review as USB. ACCEPT on the last page wrote
  `LEEKZ/signed.zsig`.
- `zecz finish --pczt …/unsigned.pczt --sigs …/signed.zsig` applied one
  signature (verified against rk), proved, and broadcast **txid
  30cdefb47286f2e3397dd13eaaba88fa772910ca41578a99baaf271adab993a4**.
- The `.zsig` the C firmware wrote parsed in zecz's independent Rust
  reader. The hash in it matched the PCZT on the card.
- Needed for this: FATFS long names on the board (`CONFIG_FATFS_LFN_HEAP`).

### 6 Android app: USB, Bluetooth and QR signing on the board (2026-10-16)
- The Zodl fork (`android/`, branch `leekwallet`, app "LeekWallet Zcash")
  imports the board's account and signs testnet sends three ways, all
  confirmed by the user:
  - **USB OTG**;
  - **Bluetooth LE**;
  - **QR**: `ur:leek-pczt` to the board, `ur:leek-zsig` back.
- Fixed on the way:
  - With BLE up, the 64 KB Zcash worker stack no longer fit in internal
    RAM. The worker now has 24 KB for the C backend, and the QR scan
    buffers moved to PSRAM.
  - The top bar and the review screen said Keystone for a LeekWallet
    account.
- QR bench, phone to board:
  - A signer-view PCZT is ~2.8 KB and does not compress (zlib saves 15
    bytes: ciphertexts and commitments).
  - Best settings: board at 320x240 with exposure -4 (LeekWallet's
    defaults, unchanged), tablet at 500 ms, 50-byte fragments (~50
    frames). 90 bytes also works. 150 and 250 did not read reliably.
  - The return QR, the .zsig of ~110 bytes, is one frame.
- Recommendation:
  - USB or Bluetooth for everyday signing.
  - QR (or the SD card) when the device must stay off every link.
- Future, to halve the QR: a "compact PCZT" without the note
  ciphertexts, which the board can rebuild from recipient, value, rseed
  and memo (~1.4 KB).

### 7.1-7.2 Compact PCZT: signed on the board over USB (2026-10-16)
- `zecz send --compact` writes the compact signer view
  (`SignerView::Compact`, v2 PCZT). On the board's wallet, a send with a
  memo plus change: **1,430 B** instead of ~2.8 KB.
- The board signed it unchanged: the C signer rebuilds the left-out
  fields. `finish` applied the signatures to the kept full PCZT, proved,
  and broadcast testnet tx
  `f7f6ad4b01be8650c9dc5f78e7d844133e5f53c4daf6b3cc1131a802bae2f2cb`.
- `leek-android`: `compact_view` (the same redactions with the pczt
  Redactor) and `apply_signatures_to(pczt, sent, zsig)`, which refuses a
  sent view of another transaction (sighash). Tests prove, extract and
  verify the result. The app sends the compact view over USB, Bluetooth
  and QR, with a "PCZT: compact / full" switch on the sign screen.
- Broke on the way: the first attempt expired (`expiry height` exceeded).
  A PCZT lives ~40 blocks, and the board was on its Bluetooth link, so
  the USB request went unanswered until it was switched. Not a compact
  issue; build the PCZT just before signing.

### 7.4 Compact PCZT in the app: USB, Bluetooth and QR on the board (2026-10-16)
- The user signed with the app over all three links with the compact
  view on (the default).
- QR at 50-byte fragments: 28 frames, about half of before. Still slow
  next to USB and Bluetooth, so the docs now recommend USB or Bluetooth
  in a trusted place for everyday use, and QR or SD only when the board
  must stay off every link.
- The Android app is now pushed to a private repo,
  github.com/0xOucan/leekwallet-zcash-android (branch `leekwallet`,
  default; `upstream` = zodl-inc/zodl-android).

### 8.1 zecz library split (2026-10-16)
- New crate `host/zecz-core`, lib name `zecz`: the old library modules
  plus `zecz::wallet`, typed functions that return data and never print:
  `Profile` (data dir, network, server), `Wallet::create/open/init`,
  `sync` with a progress callback, `tip`, `balances` (Orchard, Ironwood,
  transparent: total, spendable, pending), `addresses`, `propose_send`
  (payments, change, fee, target height, signer view; the full PCZT goes
  to `pending/`) and `finish` (apply `.zsig`, prove, extract, store,
  broadcast; returns the txid). `zecz::seed_card` covers `.zkv` cards.
- `host/zecz-cli` is now only the `zecz` binary, over that API, with no
  direct librustzcash dependency. leek-android's tests use `zecz-core`.
- Same output as before: the offline commands (help, errors, `zkv-new`,
  `zkv-open`, `sign-soft`) diffed against the old binary, and `tip`,
  `init-dev`, `sync`, `balance`, `address` and a refused `send` on
  testnet. One change: `finish` on a data dir with no wallet now says
  "no wallet" rather than "no pending PCZT".
- Tests: `zecz-core/tests/wallet.rs`, 9 offline (temp-dir profiles, the
  abandon-art account imported from an offline birthday and checked
  against `vectors/keys.json`, send and finish refusals, `.zkv`) and 2
  `#[ignore]`d on testnet (tip; init and sync near the tip), which pass.

### 7.3 Compact PCZT vectors (2026-10-16)
- New valid vectors, made as the app's `compact_view` makes them (a host
  test checks byte equality and the full view's sighash):
  `iw_send_transparent_compact` 1,286 B (full 4,730),
  `iw_send_nu7_compact` 1,373 B (4,807),
  `orchard_to_ironwood_compact` 3,189 B (9,450, both bundles compacted).
- Finding: the compact view carries nothing that commits to the memo.
  cmx and epk do not cover it, and the ciphertext is rebuilt from it. So a
  changed memo is a different transaction, not one the device can refuse.
  `iw_send_shielded_compact_memo_changed` (3,507 B) pins that: the device
  shows the new memo and signs a different sighash. Its signatures verify
  for neither backend's view of the original, and `apply_signatures_to`
  refuses them.
- New refusals: `r_compact_recipient_tampered` (pk_d changed; with cmx
  absent only user_address catches it, -14) and
  `r_compact_diversifier_tampered` (the memo re-encrypts to another epk,
  -10). 20 PCZT vectors in all. The generator is seeded: every existing
  file is unchanged, so `iw_send_shielded.zsig` still matches.
- `test_zsign_diff`: the random pass now covers the 5 compact vectors
  (6,225 mutations, 3,429 accepted, 0 mismatches). A new pass aims at the
  compact output fields (memo, recipient, rseed, epk, rcv, user_address):
  488 mutations, 193 accepted, and every accepted one changed the sighash.
- `make -C firmware/sim test` and `cargo test` in `host/` (33 tests)
  pass.

### 8.1 Desktop skeleton (2026-10-16)
- `desktop/`: the Tauri v2 app "LeekWallet Zcash", built from the
  structure of `firmware/app` (pnpm, Vite, plain TypeScript over the
  global Tauri API, the same design tokens and a subset of its styles)
  without its EVM code. One screen: network and server, an "import
  viewing key" form (UFVK and birthday, or the chain tip), Sync with a
  progress bar, the balance per pool and the unified address.
- Backend: `src-tauri/src/backend.rs` wraps `zecz::wallet` as blocking
  calls with serializable results; `lib.rs` exposes `profile_status`,
  `init_watch_only`, `sync` (emits `sync://progress`), `balances`,
  `addresses` and `tip`, each on a worker thread with its own
  current-thread runtime. One sync or import at a time. The wallet is a
  zecz data dir at `<app data>/wallets/<test|main>`; testnet unless
  `LEEKZ_NETWORK=main`.
- `src-tauri` is its own Cargo workspace, not a member of `host/`, so
  Tauri stays out of the host lock. Seeding its lock from host's did not
  work: host's carries Tauri 2.3 (the optional Android feature of
  leek-transport-serial), which does not build against current
  tauri-runtime. A fresh lock resolves the librustzcash stack the same
  (pinned git rev; orchard 0.16.0, halo2_proofs 0.4.0).
- zecz-core: `AccountSpec.origin` is now an `Option<KeyOrigin>`; none
  imports a view-only account rather than inventing a ZIP 32 derivation
  no device could sign for. `Wallet::birthday` added for the progress
  count. The CLI is unchanged in behavior.
- Built here: `cargo build` in `src-tauri`, and `pnpm tauri build --debug`
  (.deb and AppImage). The app starts and draws the import screen.
- Tests: `desktop/src-tauri/tests/backend.rs`, one offline (empty
  profile, refusals, the progress JSON) and one `#[ignore]`d on testnet:
  the abandon-art UFVK imported watch-only about 2,500 blocks below the
  tip and synced; progress rose 0.40, 0.80, 1.0 and it found a
  10,000-zat Ironwood note. Both pass; host tests pass with a new
  offline view-only import test.
- CI: `.github/workflows/desktop.yml` builds on Ubuntu, Windows and
  macOS and runs the host and backend tests on Ubuntu. Not run yet (not
  pushed).

### 6.9 Verify address on the device, with rotating addresses (2026-10-16)
- The Receive screen has "Verify on device" for a LeekWallet account
  (USB or Bluetooth). Confirmed by the user on the board over Bluetooth.
- First attempt failed by design gap: Zodl shows a new diversified
  address each time Receive opens, and the device knew only its
  default address, so it reported a mismatch between two valid
  addresses of the same wallet.
- Fix: zShowAddress takes the app's address. The device proves it is
  the account's (Orchard receiver pk_d = [ivk] g_d, index recovered by
  FF1 decryption of the diversifier, any P2PKH receiver checked against
  the transparent key at that index) and only then shows it. Foreign,
  mixed, Sapling-bearing and malformed addresses are refused before the
  screen. Vectors from librustzcash at indices 1, 7, 2^31+5 and 88 bits.

### 8.3 Desktop hardware accounts (2026-10-16)
- The desktop app imports and spends a LeekWallet account, over USB or
  Bluetooth. Done on PC against `zdev`; the board is next.
- Backend (`desktop/src-tauri`):
  - `src/link.rs`: USB through leek-transport-serial (ports listed with
    the board's VID 303a first). Bluetooth through leek-transport-ble:
    leek-device could not use it directly (btleplug is async, `FramePort`
    is blocking), so `BlePort` runs it on a one-worker runtime, keeps the
    23-byte MTU floor, and disconnects explicitly on drop (the stale
    BlueZ subscription fix in LeekWallet's transport).
  - `src/hardware.rs`: the flows of `zecz device-init`,
    `device-show-address`, `send --compact`, `sign-device` and `finish`.
    The birthday comes from the user, else from the device; one the
    device lacked is recorded there. Before verifying or signing, the app
    checks that the paired device holds the wallet's seed (new
    `Wallet::key_origin` in zecz-core), so another board is refused with
    both ZFPs before anything is uploaded. Device error codes read as
    sentences (rejected, timed out, locked, session ended, refused to
    sign with its reason), with the device's message after them.
  - Commands `list_ports`, `ble_scan`, `device_pair` (passkey on
    `device://passkey`), `device_session`, `device_disconnect`,
    `device_import`, `verify_address`, `propose_send` (compact view) and
    `sign_and_send` (steps on `device://status`, returns the txid). The
    paired session stays open between commands; one device call at a
    time; a failed link or a dropped session closes it.
- Frontend: a LeekWallet panel (link chooser, passkey, import with the
  birthday), "Verify on device" on the receive card, and a send form with
  a review (payments, change, fee, total) and "Sign on LeekWallet".
- Tests: `tests/device_zdev.rs`, 5 offline plus 1 `#[ignore]`d on
  testnet. Against zdev: pair, import, a second companion taking the
  birthday from the device, verify the wallet's address and refuse a
  foreign one, sign `iw_send_shielded_compact` and prove and extract the
  result with the full PCZT; a rejection on the device; a wallet of
  another seed. All pass, with the existing backend and zecz-core tests.
- CI: `libdbus-1-dev` added for btleplug on Linux. Bluetooth compiles but
  is untested here (no radio, no board).
- Left: the board over USB and Bluetooth, a broadcast send from the app,
  and `.zkv` card creation (with 8.4).

### 10.1 Transparent inputs (2026-10-16): done on PC, board pending
- Design in FORMATS.md ("Transparent inputs") and PROTOCOL.md. Each
  input must be plain P2PKH, SIGHASH_ALL, with one BIP 44 derivation
  naming this seed (ZIP 32 fingerprint), coin type and account, scope
  0/1/2; the device re-derives the key at that path and checks HASH160
  against the script. Refusals: not ours -12, no derivation -19, other
  shapes (P2SH, SIGHASH_NONE, script_sig, lock times) -16, pczt parse
  rules -10. The fee counts the inputs; their amounts and scripts are
  committed by every ZIP 244 sighash, so a lying host gets signatures
  the network rejects. The shielded sighash switches to the S.2
  SIGHASH_ALL form when there are inputs.
- Rust backend: zcash_transparent keys, libsecp256k1. C backend (the one
  on the chip): own transparent digests, trezor-crypto BIP 32 / ECDSA in
  `zc_secp.c`, the only zsign-c unit without the zcv_ prefix;
  `check-zsign-c-symbols.sh` allows trezor references from it alone
  (esp32s3cam: 156 zcv_* symbols, none shared). Both use RFC 6979, so
  their transparent signatures are byte-identical.
- Memos of outputs paid from transparent inputs are recovered with the
  account's transparent OVKs (ZIP 316), as wallets encrypt them.
- `.zsig` version 2 only when a transparent entry exists (DER ‖ 0x01,
  pubkey); a shielded-only file is byte-identical to version 1, so
  `vectors/zsig/iw_send_shielded.zsig` still applies. zSignFinish carries
  the same fields; zGetFeatures lists transparent. QR return message cap
  raised to 1024 B (a shield of 2 inputs is 263 B).
- Review: a "From k of n / Your t-address / Path .../s/i" page per input
  before the recipients, "From t-addr: n" on the fee page, "Shield X into
  this wallet" when nothing leaves.
- Host: leek-device parses v2 and the protocol fields; zecz-core and
  leek-android add the pubkey as hash160 preimage and use
  `Signer::append_transparent_signature`; `propose_send` accepts
  transparent inputs; `zecz shield` / `propose_shield`.
- Vectors (12 new, 32 in all, existing files unchanged): t_shield,
  t_mixed, t_send_tovk, two compact views; refusals r_t_input_not_ours,
  r_t_wrong_path, r_t_wrong_account, r_t_missing_derivation,
  r_t_foreign_seed, r_t_value_tampered (-15), r_t_sighash_none.
- Found by test_zsign_diff's new transparent pass: zcash_script refuses
  disabled opcodes even in a spent coin's (FromChain) script; the C
  parser accepted them (-16 instead of -10). Fixed; the pass is the
  regression test.
- Numbers: test_zsign_diff 9,750 random mutations (5,333 accepted, 0
  mismatches) + 3,363 on every transparent input byte (722 accepted, 0
  mismatches); it now takes ~17 min. `make test` passes; `cargo test` in
  host/ passes (57, 2 ignored), including zdev signing t_shield and
  t_mixed_compact end to end: applied, proven, extracted, verified.
  esp32s3cam builds (flash 80.6%).
- Open: the board (shield review pages on the panel, timing of five
  secp256k1 derivations per input), a testnet `zecz shield`, the Android
  app UI for shielding, and the Chrome/desktop companions.

### 8.3 Desktop with the board: tested on the board (2026-10-16)
- USB: pair, import (birthday from the device), sync, verify address,
  and a send signed on the board: tx
  `2c6a4ff856f554837d90eeac0e0e6b114b65a9580c941c2b35d991bb10c1eed1`.
- Bluetooth: first attempt panicked ("the worker thread panicked"):
  bluez-async spawns a cleanup task when its message stream is dropped,
  and the scan results and the closed transport were dropped outside
  the Tokio runtime. Fixed by keeping every btleplug object inside it.
  Then verify address and a send over Bluetooth: tx
  `b6366dccdbed53d7cb8dd0ffa00870b2d572c33fbeeb4dc51d7c3d616a6f22a9`.
- Also funded the board's t-address (tx
  `dbfce8b9edd0bf33faa1287e2de417c8359eda0cb0bb45bc198d5e4dccfb9232`)
  for the shield test of 10.1.

### 10.1 Transparent inputs: first shield signed on the board (2026-10-16)
- The board's t-address held three coins (0.0102 TAZ). `zecz shield
  --compact` built a 1,739-byte compact PCZT moving them into Ironwood
  (0.00995 TAZ, fee 0.00025). The board showed one "From k of n" page
  per input with the t-address and path, and "Shield ... into this
  wallet"; it signed the three transparent inputs, and the transaction
  proved and was accepted: tx
  `d18f66149acf36b9d932b062ae8e8383bbb1cf2d28fab648bfb27054f1366191`.
- Found on the way: with only 0.0002 TAZ on the t-address, shield built
  a transaction whose whole balance went to the fee (ZIP 317 fee plus
  dust change absorbed into it). `propose_shield` now refuses that.
- zecz printed transparent signatures as "action"; they are inputs.

### 8.4 Desktop wallet screens (2026-10-16): done on PC, board next
- Receive: the address as a QR code (SVG from the `qrcode` crate,
  pinned at 0.14.1 without its image feature, drawn by the backend; black
  on white in both themes, CSP unchanged: a `data:` image). "New
  address" calls zecz-core's new `Wallet::next_address`
  (`get_next_available_address`): Orchard + P2PKH while the transparent
  gap limit allows (10 unused external addresses; sync watches the gap
  addresses' coins), then Orchard only rather than a transparent
  receiver the wallet would not watch. The first un-exposed gap index can
  be 0, the default address, so it is skipped. The rotated address is
  kept for the session; "Verify on device" takes it as is. The
  transparent address sits apart under a PUBLIC warning.
- Shield: `propose_shield` in the desktop backend over zecz-core's
  (compact signer view); the balance card shows "Shield transparent
  funds" when the transparent balance is above zero; the review shows
  the amount, fee, pool and total; signing is the send's
  `sign_and_send` (the two flows share one function in main.ts now).
  Refusals are shown as zecz words them. Found while testing: with one
  coin the ZIP 317 fee is 15,000 zat (1 transparent input, padded
  shielded output); below it zecz says "Insufficient funds"; at exactly
  the fee the "shielding would move nothing" refusal fires; a coin of
  17,000 is refused as short of a 20,000 fee (zcash_client_backend's
  dust handling).
- History: zecz-core `Wallet::history(limit)` reads `v_transactions`
  (kind from `is_shielding`, `pool_crossing_value` and the balance
  delta) and `v_tx_outputs` (recipients outside the wallet, text memos
  without change). A fully shielded transaction states its fee, so even
  a received one has it; the app shows a fee only for what this wallet
  paid. `Wallet::store_transaction` (decrypt_and_store_transaction)
  stores a raw transaction, used by the tests.
- Contacts: `src-tauri/src/contacts.rs`, `<app data
  dir>/contacts-<test|main>.json`, versioned, written atomically; names
  unique ignoring case; addresses checked with zecz-core's new
  `check_address`. A picker in the send form, "Pay" on each contact,
  contact names in the history. Delete takes two presses (no
  `window.confirm` in every webview).
- Account index: still 0 in the UI; nothing new depends on it.
- Tests, all passing: zecz-core `tests/receive.rs` (rotation through
  the gap limit to Orchard only, each address re-derived from the UFVK;
  address check), `tests/history.rs` (the `iw_send_shielded` vector
  soft-signed, proven, extracted and stored in the payee's and the
  payer's offline wallets: received 400,000 zat with its memo; the
  payer's recipient and memo via its OVK), `tests/shield.rs` (a chain
  tip, one scanned empty block and one coin put into an offline wallet:
  refusals at 5,000, 12,000 and 15,000 zat, a 1,000,000-zat shield
  built into Ironwood). Desktop `tests/backend.rs`: history and its
  JSON, rotation and QR, shield refusals, contacts. The ignored testnet
  test also lists the history: two transactions found near the tip.
  `cargo test` in host/zecz-core and desktop/src-tauri pass (zdev tests
  included); `pnpm typecheck` and `pnpm build` pass.
- Left: on the board, a shielding signed on the device (the board's
  t-address holds 20,000 zat, which zecz found the fee would take whole
  (45518f9), though one 20,000-zat coin builds offline here: send it
  about 0.001 TAZ more for a shield that surely moves value), a payment
  to a rotated address found by sync; `.zkv` cards.

### 9.1 Browser engine spike (2026-10-16)
- Decision: the extension's engine is our own librustzcash rev
  (`16f0723c`) compiled to WASM, not WebZjs. WebZjs is pinned to
  ChainSafe's `librustzcash-nu61` fork (NU6.1, head 2026-02-02), with no
  NU6.3 and no Ironwood. Findings, numbers and the revised plan are in
  `docs/zcash/CHROME-ENGINE.md`.
- `chrome/engine-spike` builds the zecz-core crates for
  wasm32-unknown-unknown, `zcash_client_sqlite` included. rusqlite 0.37
  cannot build bundled SQLite there, so a `[patch.crates-io]`
  `libsqlite3-sys` stand-in re-exports `sqlite-wasm-rs`. The threads
  build also needs `-matomics -mbulk-memory` for that C code. Other
  fixes: tonic `=0.14.2` for gRPC-web trailers, the wasm-bindgen-rayon
  worker spawned from the packaged file instead of a blob: URL (MV3
  CSP), uuid `js`, and a `Date.now()` clock.
- Single-threaded, `sync::run` spins forever at the first block with
  shielded outputs: trial decryption goes to `rayon::spawn_fifo`, and
  the caller blocks on its result. With threads the caller blocks in
  `Atomics.wait`, which only a worker may do. So the engine runs in a
  dedicated worker started by the offscreen document.
- Measured in Playwright's Chromium 153, with the spike as an MV3
  extension, `crossOriginIsolated`, on an i7-1355U laptop. Worker, 8
  threads, abandon-art account, through ChainSafe's testnet gRPC-web
  proxy:
  - sync of 8,501 blocks in 22 s, which found the 10,000-zat Ironwood
    note;
  - propose plus PCZT of a 0-zat send to self in 0.3 s (4,946 B);
  - proving key 9-13 s, and a 2-action proof in 9-10 s;
  - sign, prove and extract gave a 9,166 B v6 transaction, not
    broadcast;
  - a 50,001-block sync took 141 s (about 355 blocks/s), and the
    database grew to 4.6 MB (1.8 MB after 8,500 blocks);
  - 145 MB of WASM memory; 13.0 MB WASM, 4.3 MB gzip.
- Proving a 2-action bundle: single-threaded in a tab or offscreen
  document took 56-60 s (key 44 s), 4 threads in a worker 16-20 s.
  Natively, 4.8 s single and 2.2 s multi-threaded.
- gRPC: zec.rocks speaks only plain gRPC. ChainSafe's testnet and
  mainnet proxies speak gRPC-web. A `fetch` from an extension page to
  zec.rocks does read the gRPC messages, but not the trailer status, so
  an error looks like an empty 200.
- Not done: broadcast, OPFS persistence, wasm-opt, and sync from an old
  birthday.

### 10.3 Mainnet readiness (2026-10-17): done on PC, the board send pending
- Audit: coin type 133 and the u1 / uview1 / t1 encodings were already
  right in both signers, zcash-account, zecz-core and zecz; the device
  takes the network per request and the PCZT names it only by its
  global coin type (branch ids are shared), which both signers check
  (-17). Mainnet lightwalletd (zec.rocks) is on NU6.3 (37a5165b, tip
  ~3,520,580), in ZCASH_SIGN_BRANCHES.
- Bug found: `zecz --network main --server <testnet>` sync went ahead
  and started storing testnet blocks in the mainnet wallet; it stopped
  only on a commitment-tree root conflict. zecz-core now asks
  GetLightdInfo and refuses a server whose `chain_name` is not the
  wallet's before init, sync and broadcast (`check_chain`,
  `broadcast_on`); `zecz tip` warns.
- Vectors (8 new, 40 in all; the 32 existing files regenerate
  byte-identical): `main_iw_send_shielded`, `main_t_shield` and their
  compact views at a mainnet NU6.3 height, and `r_main_on_test`,
  `r_main_shield_on_test`, `r_test_on_main`, `r_test_shield_on_main`
  (network_mismatch).
- Sim: zShowAddress on mainnet through the real vault (u1 ours, account
  1 foreign, a testnet address malformed, the coin type 1 receiver as
  u1 foreign) and through the protocol (shown as MAIN); every vector's
  review names its network and unit (Mainnet/ZEC, Testnet/TAZ).
  `make -C firmware/sim test` passes; test_zsign_diff 12,450 mutations,
  0 mismatches.
- Host: `tests/mainnet.rs` (account, addresses, chain check, mainnet
  vectors soft-signed, proven, extracted); zdev signs the mainnet compact
  vectors for "main" (applied to the full PCZTs, proven, extracted),
  exports and confirms the u1 account, and refuses each cross-network
  PCZT with -17.
- Mainnet sync, run with `--ignored`: the public test account
  (abandon-art, `main/abandon-art/pp0/acct0`) imported watch-only 1,000
  blocks below the tip synced to 3,520,585 in 12.7 s; balances 0;
  propose_send refused "Insufficient balance (have 0, need 20000
  including fee)", shield "Insufficient funds"; a testnet server refused
  for a mainnet wallet. With the CLI from birthday 3,520,000: 579 blocks
  in 4 s, the same refusals.
- Desktop: "Network and server" switch (Testnet default, Mainnet after
  a confirmation), saved in `settings.json` with a server per network;
  wallets/test and wallets/main and the contacts stay apart; a switch
  closes the device session and drops a pending send; a session paired
  for one network is refused for the other. On mainnet: a red
  "MAINNET — real funds" bar and a tick box after the review before
  Sign (also enforced by `sign_and_send`). Tests: `tests/settings.rs`
  (5) and a zdev mainnet flow; `pnpm typecheck`, `pnpm build` and
  `cargo test` pass.
- Checklist for the board: `docs/zcash/MAINNET.md`. Left: the first real
  mainnet receive, address check, send and shield on the board.

### 9.2 Chrome extension engine (2026-10-17)
- `chrome/engine`: the spike turned into the extension's engine. Our
  librustzcash rev (`16f0723c`) for wasm32 with threads, in a dedicated
  worker, driven by JSON messages (`chrome/README.md` lists them):
  open a wallet per network, import a UFVK (view-only, or a device
  account with its ZIP 32 origin, as zecz's `AccountSpec::device`),
  sync with progress and cancel, balances per pool, addresses with
  rotation, history, propose a send or a shield (compact signer view
  out, full PCZT kept), apply a `.zsig`, prove with progress, extract,
  broadcast over gRPC-web.
- Shared code: `host/leek-device/src/zsig.rs` (`.zsig` v1/v2) and
  `host/zecz-core/src/{authorize,keys,soft_signer}.rs` compile for wasm32
  unchanged, included with `#[path]`. zecz-core's `wallet.rs` needs
  `std::fs`, tonic's transport and tokio, so its operations are restated
  in `chrome/engine/src/wallet.rs`, each naming the zecz function it
  follows.
- Storage: `zcash_client_sqlite` on OPFS through `sqlite-wasm-vfs` 0.3
  (the sync access handle pool) under the `libsqlite3-sys` stand-in,
  which moved to `chrome/engine/patches`. Pending and sent PCZTs go in
  a second database beside the wallet (zecz keeps them as files).
- Found: `sync::run` awaits the block cache's `delete`s only when the
  whole run ends. Progress reported from `delete` came all at once at
  the end, and every downloaded block stayed in memory. The cache now
  reports, and drops, the previous batch when the next one arrives.
- `chrome/extension`: MV3 skeleton. The service worker only creates the
  offscreen document; that document starts the engine worker and serves
  it to pages over a runtime port, and restarts it if the WASM traps.
  COOP/COEP in the manifest. Popup after LeekWallet's EVM extension:
  network switch, import a viewing key, sync progress, balances per
  pool, receive address with rotation, recent history, endpoint setting.
  Plain ES modules, no bundler.
- `test/e2e.mjs`, headless Chromium (Playwright's, Chromium 153), i7-1355U,
  8 threads, ChainSafe's testnet proxy, abandon-art testnet account 0
  imported as a device account with birthday 4,501,000: 26/26 checks.
  - Engine start (WASM, 8-thread pool, OPFS) 0.3-0.9 s; import 0.4-0.8 s.
  - `cancelSync` stopped a sync after its first 1,000-block batch; the
    next sync went on from there.
  - Reloading the extension in the middle of a sync lost nothing: same
    account, and the sync finished afterwards (about 7,700 blocks in
    35-54 s, 144-201 blocks/s, to tip 4,510,727).
  - Balances: 10,000 zat, all Ironwood, spendable (the note 9.1 found).
    History: 2 transactions. Rotation gave index 1, then 2 after a reload.
  - After an extension reload and after a browser restart: same
    account, same scan height, same balances, and the next sync scanned
    0-2 new blocks in 1-4 s.
  - Wallet database 1.92 MB, extension database 41 kB, 33 MB of WASM
    memory (no proving key built). Engine 13.9 MB of WASM, no wasm-opt.
  - A send to self cannot be proposed: 10,000 zat does not cover 1,000
    plus the 10,000 fee. The engine says so cleanly.
- Reloading an unpacked extension in Playwright's Chromium needs
  developer mode on (`chrome.developerPrivate`); otherwise Chromium
  disables it on reload ("unsupportedDeveloperExtension").
- Not done: broadcast through the proxy (opt-in `BROADCAST=1` with the
  dev soft signer, needs a funded test account), the proving key in the
  extension (`warmProver`, `prove` are in the API, not exercised here),
  wasm-opt, a sync from an old birthday, the board (9.3), software
  wallets (9.4).

### 10.3 Mainnet: one birthday per network, and the board paired on mainnet (2026-10-17)
- Found before the first mainnet import: the board kept one birthday per
  account whatever the network, so recording the mainnet birthday would
  have overwritten the testnet one, and a testnet height (far above
  mainnet's tip) served on mainnet would hide every note. The firmware
  now keys records by account and network (testnet keeps the old NVS key,
  so existing records still read); `zSetAccountMeta` takes `network`;
  every companion passes its own. Tested in test_zcash_meta and via zdev;
  flashed to the board.
- The board paired with `zecz --network main` over USB, the account was
  imported at birthday 3,520,600 (same ZFP as on testnet: one seed, coin
  type 133 instead of 1), and the mainnet birthday was recorded on the
  board next to the testnet one. The board's own Receive screen, switched
  to Mainnet, shows the u1/t1 addresses zecz derived.
- Next: receive a small amount, send some back and shield, all signed on
  the board (docs/zcash/MAINNET.md). Real mainnet addresses and txids are
  kept out of the repository on purpose.

### 10.3 Mainnet: first send and shield signed on the board (2026-10-17)
- The user funded the board's mainnet account (shielded and transparent).
- `zecz --network main` sent a small amount to the account's own
  address with a memo (compact PCZT, 1,452 B), signed on the board over
  USB, proved and broadcast: accepted on mainnet.
- Then a shield of the transparent coin into Ironwood (compact PCZT,
  1,426 B): the board showed "From 1 of 1", the t1 address and its path,
  and "Shield ... into this wallet"; signed, proved, accepted.
- The board's review named Mainnet and ZEC throughout. Both txids are
  known to the user and deliberately not recorded here.
- Fixed: `zecz balance` still said the device could not sign transparent
  inputs.

### 9.3 Chrome extension with the board (2026-10-17)
- `chrome/extension/src/board/`: the board's protocol in plain ES modules,
  the JavaScript twin of `host/leek-device/src/device.rs`, with
  LeekWallet's `session.ts`, `framing.ts` and `serial-transport.ts` as
  the source for the session rules and the Web Serial lessons (DTR/RTS
  low after open, boot log drained, `reader.cancel()` before close).
  - Crypto: X25519, SHA-256 and HKDF from WebCrypto (X25519 needs
    Chrome 133, now the manifest's minimum); ChaCha20-Poly1305, which
    WebCrypto lacks, from `@noble/ciphers` 1.3.0 (LeekWallet's version),
    copied from node_modules at build time (only relative imports, so
    still no bundler).
  - The host's send counter moves only when a reply opens, and the
    session refuses to seal a different message under a held counter. A
    timeout or a dropped link ends the session (no request ids: a late
    reply would pair with the next request); a plaintext 0x0401 resends
    the same frame.
  - BLE: 1-byte-header chunks at MTU 23 (Web Bluetooth does not expose
    the negotiated MTU), written with response.
- Where it runs (MV3):
  - USB in the offscreen document: Web Serial needs a document and the
    popup closes on blur. `requestPort()` needs a click in a tab (the
    toolbar popup closes when the chooser takes focus), so the first USB
    connection opens the wallet in a tab; then the offscreen document's
    `getPorts()` finds the board (VID 0x303a). `board.<op>` calls go over
    the engine port, the state comes back as `{ board }`.
  - Bluetooth in the tab that chose the device: a `BluetoothDevice`
    cannot move between documents, and `navigator.bluetooth.getDevices()`
    is still behind a flag. The tab stays open while it signs.
  - `BoardHost` (`host.js`) is the same in both: connect (passkey, ALLOW,
    PIN), import (zGetAccount -> importAccount with the ZIP 32 origin;
    the board's birthday for the network, or ask and record it with
    zSetAccountMeta incl. `network`), verify (zShowAddress with `ua`),
    sign and finish (zSign* on the compact view -> `.zsig` -> the
    engine's finish: apply, prove, extract, broadcast). Mainnet needs
    the real-funds tick, and the engine must have the same network open.
- Popup: Board section (USB, Bluetooth, the six digits, PIN), Import
  the board's account, Verify on the board, a send form and Shield when
  there are transparent funds, a review (payments, fee, change), the
  mainnet tick like the desktop app's, progress, the txid.
- Tests, all without a board:
  - `node --test test/board.test.mjs`: 23/23 against zdev, about 7 s.
    Over the USB framing: the passkey equals zdev's, both networks'
    accounts equal `vectors/keys.json`, a birthday per network, address
    confirmed and a foreign one refused (-14), compact and full sends,
    a shield with transparent signatures (`.zsig` v2), mainnet, refusals
    (-13 and a network mismatch) with the session surviving them, a
    rejection on the "device", a refused pairing. Over Bluetooth through
    a fake GATT server chunking at MTU 23 (writes of at most 20 bytes): a
    mixed compact send. `BoardHost` with a stand-in engine. The `.zsig`
    writer re-encodes `vectors/zsig/iw_send_shielded.zsig` byte for byte.
  - `node test/e2e.mjs`: 35/35 (the 9.2 checks plus 9). Step 5b pairs
    with zdev from an extension page through a bridge to its PTY (1.2 s),
    so WebCrypto X25519 and the vendored cipher run in Chromium against
    the firmware's protocol: the board's account imports as the same
    device account (birthday asked, then recorded, then read back), the
    engine's default address is the board's, a rotated address (index 1)
    is confirmed and another account's refused. The popup shows the
    board, send and verify controls, and Connect over USB with no port
    granted asks for the chooser.
  - CI: a `board` job in `chrome.yml` builds zdev and runs the Node tests.
- Not done: a send from the extension signed on a real board (the
  abandon-art account holds 10,000 zat, below amount plus fee, so not
  even zdev could sign a real proposal here; `BROADCAST=board` does it
  given funds); QR through the webcam; accounts other than 0.
- To try on the board, testnet first (the board's link set to USB):
  1. `chrome/engine/build.sh`, then in `chrome/extension`: `npm install`,
     `node build.mjs`; load `chrome/extension/dist` unpacked.
  2. Popup: Testnet. Connect over USB: the first time a tab opens; press
     "Choose the board (USB)" and pick the Espressif port. Compare the
     six digits, press ALLOW on the board, enter the PIN there.
  3. Import the board's account (the board already has the testnet
     birthday from zecz; otherwise enter it). Wait for the sync.
  4. Receive: Verify on the board; press New address and verify again.
  5. Send a small amount to yourself with a memo: Review, Sign on the
     board, read the pages and ACCEPT. The popup shows the txid after
     proving (about 10 s) and the broadcast. Close the popup during the
     review once: the signature still lands (the offscreen document holds
     USB), and the txid shows when it reopens.
  6. Shield: when transparent funds show, Shield, sign on the board.
  7. Bluetooth: set the board's link to Bluetooth, press Bluetooth in the
     popup (a tab opens), choose the board, then steps 4-6 from that tab.
  8. Mainnet the same way with a tiny amount, after testnet works: the
     red bar, and Sign stays refused until the real-funds box is ticked.

### 8.6 Desktop NEAR Intents (2026-10-17): done on PC, the mainnet swap pending
- What 1Click is, from its OpenAPI (`1click.chaindefuser.com/docs/v0/openapi.yaml`,
  v0.1.10), its docs (docs.near-intents.org: Making a Request, API Keys,
  Confidential Swaps) and Zodl's `NearApiProvider.kt` / `NearSwapDataSource.kt`:
  - `GET /v0/tokens`, `POST /v0/quote`, `POST /v0/deposit/submit`
    (optional, speeds things up), `GET /v0/status?depositAddress=`. ZEC
    is `nep141:zec.omft.near`, 8 decimals.
  - Amounts are base-unit strings; slippage in basis points; deadline in
    RFC 3339. `dry: true` prices without a deposit address.
  - From ZEC Zodl asks EXACT_INPUT, into ZEC FLEX_INPUT, both
    `ORIGIN_CHAIN` / `DESTINATION_CHAIN`, refunds to a fresh shielded
    address of the wallet; the deposit is a plain send with an empty memo.
  - The key is optional: without one 1Click adds a 0.25% fee (and the
    answer carried a 20 bp `appFees` entry it added itself). Zodl ships
    its partner JWT in the APK; we do not copy it. The app reads
    `LEEKZ_1CLICK_API_KEY` or `oneClickApiKey` in settings.json.
  - `confidentiality: "basic"|"advanced"` is a plain field of an
    ORIGIN_CHAIN quote, but 1Click answers 401 "User authentication is
    required for confidential intent quotes" without a key. The option
    is in the app, off and greyed without a key.
- Checked live with dry quotes only (nothing created, nothing paid):
  0.01 ZEC to USDC on Solana (EXACT_INPUT) and 10 USDC to ZEC
  (FLEX_INPUT); a unified Orchard-only address is accepted as `refundTo`
  and as `recipient`. 0.001 ZEC gives a 500; 0.00132 ZEC is the minimum
  1Click names for that route. A refund costs about 0.00032 ZEC.
- Not known yet: the kind of ZEC deposit address (a dry quote has none;
  we did not ask for a real one). The app pays a unified or transparent
  one and refuses a TEX one (two-step send). The first mainnet swap will
  tell.
- Built: `src-tauri/src/near.rs` (client over ureq/rustls-ring, quote
  checks, state machine, `swaps-main.json`), commands `swap_info`,
  `swap_tokens`, `swap_quote`, `swap_execute`, `swap_status`, `swap_list`,
  `swap_forget`; `sign_and_send` records the deposit txid with its swap.
  zecz-core gained `next_shielded_address` (fresh Orchard-only, no gap
  limit). The Swap screen: preview, quote, review, sign on LeekWallet,
  a timeline polled every 20 s. A quote 1Click echoes for another swap
  (assets, amount, recipient, refund) is not used.
- Tests: `tests/near.rs` (13 offline against recorded answers, one
  ignored live dry quote, which passed), zecz-core `receive.rs`.
- Next: a tiny mainnet swap from the app with the board
  (docs/zcash/MAINNET.md, "A tiny NEAR Intents swap").

### 9.3 Chrome extension on the real board, USB (2026-10-17)
- Brave 152 (native, not the Flatpak), the extension loaded unpacked from
  `chrome/extension/dist`, the board on USB (`303a:1001`).
- Testnet: paired, imported account 0 (ZFP 551A1FD4, the board's testnet
  birthday 4,506,700), synced to the tip through ChainSafe's gRPC-web
  proxy: 0.12349 ZEC, all Ironwood, with its send and receive history.
- Mainnet: switched the popup and the board, imported (birthday 3,520,600),
  synced; "Verify on the board" confirmed the shown address. A 0.0001 ZEC
  send to the wallet's own address with a memo: the review showed amount,
  fee and Ironwood change, Sign stayed off until the real-funds box was
  ticked, the board signed (one signature), the extension proved and
  broadcast, and the network accepted it. The txid is kept out of the repo.
- Nothing needed fixing.
- Left: Bluetooth (Web Bluetooth behind `brave://flags/#brave-web-bluetooth-api`),
  a testnet send over USB, QR.

### 9.3 Board data hidden without the board (2026-10-17)
- From the user's run on the board in Brave 152 over USB (testnet and
  mainnet sends signed there), four things to fix.
- The popup showed the board account's balance, addresses, history and
  send form after Disconnect, or with the board gone. Decided: a device
  account is shown only while a board whose seed fingerprint matches it
  is connected. `BoardHost` now reads the fingerprint on connect
  (`zGetAccount` of account 0; one more key derivation on the board, a
  few seconds) and the popup filters accounts by it. Without the board:
  the board section and "Connect your LeekWallet board to see this
  wallet.", nothing else, not even the last txid signed. The data stays
  on OPFS and keeps syncing; the popup's cache is read back only for the
  account it was written for, so a reconnect is instant. Another board
  (zdev with a passphrase in the test) sees none of it and is offered
  its own import.
- "Forget this board": the engine had only `wipe` (a whole network), so
  it gained `deleteAccount` over zcash_client_sqlite's `delete_account`,
  which removes one account and the transactions only it touched, plus
  its proposals and extracted transactions in `leek.sqlite`. Offered
  while the board is connected, with a confirmation; for a lost board,
  Settings → Delete the network wallet.
- Bluetooth greyed out: the button was disabled whenever the popup had
  no `navigator.bluetooth`. In the popup it now only opens the tab, which
  checks and says how to enable it. On Linux Chromium keeps Web Bluetooth
  behind `#enable-experimental-web-platform-features` (Web Bluetooth CG
  implementation status), Brave on top of its own
  `brave://flags/#brave-web-bluetooth-api`: likely why it stayed off for
  the user with Brave's flag on. Headless Chromium on Linux has no
  `navigator.bluetooth` either, which the e2e test shows.
- USB to Bluetooth: a USB timeout now says "the board did not answer
  over USB within 10 s. Is it unlocked, and is its Link set to USB
  (Settings → Link)?". The session was already closed after a timeout;
  a test now shows the port released and Bluetooth connecting right
  after. A USB connection gets "Switch to Bluetooth" (ends the USB
  session, then says to change Link on the board), and the board section
  says how to switch. The tab shows the chooser's error text as is.
- "sent -0.00000000 ZEC": an unmined send to the wallet's own address
  (0.0001 ZEC, fee 0.0001). The history takes the known fee off the
  balance change when nothing went to another address, leaving 0. Once
  mined it showed -0.0001, so only the popup changed: such a line reads
  "sent to yourself" with the balance change. zecz-core has the same
  code but was left alone, as the case was not reproduced there.
- Tests: `board.test.mjs` 27/27 (new: the fingerprint, another wallet,
  USB timeout then Bluetooth, the hints), `format.test.mjs` 3/3,
  `e2e.mjs` 41/41 (step 8 drives the popup with a stand-in
  `navigator.bluetooth` bridged to zdev).
- Left: try it on the board (USB, then Bluetooth with the Linux flag).

### 9.3 Chrome extension on the real board: testnet send, Bluetooth (2026-10-17)
- Testnet over USB: a 0.0001 ZEC send to the wallet's own address with a
  memo, signed on the board, proven in the extension and accepted
  (txid `c165714353f138d51e8135fd633731c9b8e3266b0816e6655c58043efbe6c053`).
  Before it was mined the history line read -0.00000000 ZEC: the engine
  subtracts the fee from an unmined send to yourself. Now "sent to yourself".
- Bluetooth: dead at first in Brave with `#brave-web-bluetooth-api` on. On
  Linux, Chromium also needs `#enable-experimental-web-platform-features`;
  with both, the tab paired with the board and it confirmed the address.
- The board's wallet hidden without the board, and "Forget this board",
  tried on the board: the forgotten account and its data were gone.
- An unrelated Android scare: both Android apps got the board's "derivation
  failed" right after pairing, over USB and Bluetooth, while the extension
  worked. It did not come back with logs recording (the board's log showed
  pair, unlock, zGetAccount in about a second); cause unknown. Watch for it.
- 9.3 is done except QR through the webcam.

### 10.3 The board refuses the other network (2026-10-17)
- Seen by the user: the board was set to Testnet (Settings → Network) and
  the mainnet Android app read the mainnet account from it anyway. The
  setting only chose what the board's own receive screens showed; every
  request carried its own network and was answered for it. Decided, for
  safety: someone who thinks the board is on one network while the app
  uses the other can lose funds, so the board answers for its own
  setting only.
- Firmware: zGetAccount, zShowAddress, zSetAccountMeta and zSignBegin
  refuse the other network before anything is derived, with error
  `0x0528` (`ZSIGN_E_BOARD_NETWORK`, -40, the next free code after the
  intake's -39) and "the board is set to Testnet; switch it to Mainnet in
  Settings > Network, or use the Testnet app" (both networks named the
  right way round). zSignFinish checks again, since the setting can change
  during an upload, and drops the upload. The board shows "Wrong network",
  the app's network and its own for four seconds or until a button; that
  press does nothing else. The setting is one volatile byte written by the
  UI task and read by the protocol task. The error buffer went from 128 to
  160 bytes for the longer message.
- zSetAccountMeta without `network`: still testnet, not the board's
  setting. The companions that leave it out are testnet-only; filing their
  birthday under mainnet would give mainnet a birthday nobody chose. On a
  board set to Mainnet such a request is refused like any testnet one.
- Companions: leek-device's `Device::call` adds "The board is set to
  Testnet, but this app is on Mainnet. Switch the board to Mainnet in
  Settings → Network, or use the Testnet app." over the DeviceError (zecz,
  desktop, Android). The extension says the same with "or switch this
  wallet to Testnet", and the popup shows it after connect, where it only
  said that the board did not say which wallet it holds.
- zdev: `--network test|main` (or `ZDEV_NETWORK`, default test); SIGUSR1
  switches it to Mainnet and SIGUSR2 to Testnet while it runs, keeping the
  pairing and what it stored. The tests that use both networks on one zdev
  switch it as a user would. The fuzzer also got its missing
  `ui_request_show_address` stub and links again.
- Tests: `make -C firmware/sim test` passes (40 suites; new: the refusals,
  same network works, switching flips it, a switch during an upload, the
  notice on the screen); `cd host && cargo test` 73 passed, 5 ignored;
  `desktop/src-tauri` 31 passed, 3 ignored; `board.test.mjs` 29/29.
  `pio run -e esp32s3cam` builds. `e2e.mjs` not run: no `chrome/engine/pkg`
  in this checkout.
- Left: try it on the board with each companion, both settings.

### 6.14 Zcash in the LeekWallet Companion (2026-10-17)
- The Companion (firmware/app) did only EVM. It now has a Zcash section,
  first in the home accordion, on branch `companion-zcash`.
- Protocol: the Companion does its protocol work in TypeScript (the
  codec and session in packages/core, the Client in main.ts; src-tauri
  only moves frames), so the `z*` calls went there
  (`packages/core/src/zcash.ts`, the same calls, timeouts and checks as
  leek-device and the chrome twin) rather than linking leek-device into
  Rust, which would have meant a second pairing for one set of methods.
- Board tools: network switch (testnet default, mainnet behind a "real
  ZEC" warning and banner), the board's account per network, QR and
  copy, verify on the board, set the birthday, and "Sign a file": a
  `.pczt` from zecz, the desktop app or Zodl, sent as it is, signed on
  the board, the `.zsig` saved to the downloads folder through a small
  Rust command (the webviews ignore download links). Board errors are
  shown verbatim, so the coming network guard reads as the board words it.
- Wallet: zecz linked into src-tauri behind a default feature. Import,
  sync, propose, finish, contacts and the per-network server are the
  desktop app's modules compiled in by `#[path]`; `zwallet.rs` is the
  commands. The board stays in the webview's session: propose in Rust,
  zSign through the Client after checking the board holds the wallet's
  seed, finish in Rust. Screens after Zodl's (balance and pools, hide
  balances, sync progress, shield prompt, rotating addresses, send with
  memo and review, history with memos, contacts, server). Both networks
  live side by side under `<app data>/zcash`.
- Android: zecz-core cross-compiles for aarch64-linux-android with the
  NDK here (SQLite, secp256k1, ring, Halo 2) once its serial port became
  a default feature the Companion turns off. The arm64 debug APK builds
  (235 MB, the Rust library 218 MB unstripped), wallet included.
- Tests: `packages/core/test/zcash.test.ts` (golden `.zsig`, reply
  checks, chunking, amounts) in `pnpm test`; `pnpm test:zdev` pairs with
  zdev through the Companion's own framing and session and signs the PCZT
  vectors on both networks, plus a board that says no; `cargo test --lib`
  25 (the save command, the import refusals, and an online import and
  sync of the public test account against testnet lightwalletd). A
  headless run of the built page with a stand-in backend drew the
  wallet, the review and the mainnet warning without a page error.
- Left: run it on the board (desktop, then the phone over USB and
  Bluetooth); a testnet send and shield signed on the board, then a small
  mainnet one; the `.pczt` file picker and proving time on the phone; the
  wallet is only on screen while a board is connected and unlocked (the
  accordion's rule), unlike Zodl.

### 9.3 USB port released, board data hidden at once (2026-10-18)
- From the user's run in Brave 152 (Linux), two findings.
- The USB port held during Bluetooth: `fuser /dev/ttyACM0` showed Brave
  holding the port while the tab was connected over Bluetooth, and the
  board answered that connect with `0x0300 derivation failed`; after an
  extension reload Bluetooth worked. Causes found: `SerialLink.close()`
  awaited `writer.close()`, which never resolves while a write the board
  does not take is pending (its Link set to Bluetooth; `send()` had no
  timeout either), so `port.close()` was never reached; `BoardHost`
  published "disconnected" only after the link had closed, so the
  offscreen document still said "ready" too; and the popup ended the USB
  session before Bluetooth only when its own view said it was connected.
  Fixes: the writer is aborted, every close step bounded, a half-open
  port closed, a registry with `closeAll()`; a write that does not leave
  within 10 s fails with the USB timeout hint; `board.disconnect` (sent
  on Disconnect, Switch to Bluetooth, the network switch and before every
  Bluetooth connect) also closes any link left; an idle USB session ends
  60 s after the last extension page closed.
- Also found: `SerialLink`'s settle raced `reader.read()` against a timer
  and left the read pending, so on a quiet line the board's first reply
  went to a read nobody used (the pairing then timed out). On the board
  the boot log likely hid it. The pump now takes that read.
- Disconnect and Forget did not hide the wallet (mainnet, Bluetooth, tab).
  Reproduced in e2e step 9 with a Bluetooth stand-in whose
  `stopNotifications` never answers (assumed to be what Brave on Linux
  did; the old close awaited it before the state changed): on both
  networks the wallet stayed after Disconnect. Now `BoardHost` publishes
  "disconnected" first and gives the link 5 s, `BleLink` gives
  `stopNotifications` 1 s and then calls `gatt.disconnect()`. Forget hides
  the accounts at once and deletes them after the sync stops (it waited
  for the sync before). The same run found that a page opened mid-sync
  with no cache drew nothing; the offscreen document now keeps the last
  status for it.
- Tests: `board.test.mjs` + `format.test.mjs` 38/38 (new: a Web Serial
  stand-in on zdev that counts opens and closes: closed after Disconnect,
  a hung write, its timeout, Disconnect while opening, `closeAll`; the
  hanging Bluetooth stack). `e2e.mjs` 59/59 (step 9 alone: 20/20, and
  12/18 with the old `host.js` and `ble-link.js`).
- Left: try it on the board in Brave (USB, Switch to Bluetooth, `fuser`
  empty during Bluetooth; Disconnect and Forget on mainnet).

### Desktop wallet: stale transparent balance, sends to yourself, 0x0300 after connecting (2026-10-18)
Three things seen running the desktop app on testnet (account imported
from the board, ZFP 551A1FD4), on branch `desktop-wallet-fixes`.
- **Stale transparent balance.** The app showed Transparent 0.0102 TAZ
  and "Shield transparent funds" for a coin already spent: faf76b13…
  paid 0.0102 TAZ to the wallet's own t-address, and a shield made by
  another wallet with the same keys (d18f6614…, 0.00995 shielded) spent
  it. Cause: compact blocks carry no transparent inputs, so a scan never
  sees a transparent spend; `zcash_client_backend::sync::run` only adds
  UTXOs (`put_received_transparent_utxo` is an upsert and its own doc
  says spent ones are left in place), and nothing answered the wallet
  database's `transaction_data_requests`, so the full transactions that
  mark spends were never fetched. The Chrome engine runs the same
  `sync::run` and has the same gap; it showed 0 only because its account
  was imported after the shield, so it never saw the coin unspent.
  Fix: `Wallet::sync` ends with `fetch_transaction_data`: it answers the
  requests (GetTransaction → `decrypt_and_store_transaction`, status of
  unmined ones, GetTaddressTxids spend searches with
  `notify_address_checked`; the ZIP 320 ephemeral-address checks, asked
  from height 0 on every sync, go to one GetAddressUtxos call), then
  checks every unspent transparent coin of the wallet against the UTXO
  set and, for a vanished one, stores its t-address's transactions since
  it was mined. Errors are counted (`SyncEvent::Transactions`), not
  fatal. Offline tests with a chain in memory (`tests/transparent_spend.rs`:
  a shield made by a second wallet removes the coin and the prompt), an
  ignored one against testnet lightwalletd (passes). Follow-up: the
  Chrome engine (`chrome/engine/src/wallet.rs`) needs the same step; not
  touched here.
- **Sends to yourself.** A mined send to the wallet's own address read
  "−0.00000000 TAZ, Fee 0.00020000 TAZ": the history took the amount as
  the balance change minus the fee. New `TxKind::SentToSelf` with the
  balance change as amount; the desktop shows "Sent to yourself −fee"
  (as the Chrome popup does), and a new `zecz history` prints the same.
  Tested on a real t→own-t transaction built in the test, unmined and
  mined.
- **0x0300 right after connecting.** Over Bluetooth the desktop got "no
  wallet on the device: create or import one" (a retry worked); Android
  and Chrome got "derivation failed" right after pairing over BLE.
  Analysis of protocol.c, leek-wallet.c and the UI:
  - A real race, fixed: `wait_for_wallet()` took `unlocked` and a
    selected wallet for ready. But `wallet_unlock()` sets `unlocked`
    before it loads the wallet metadata, migrates an old vault and
    decrypts the active seed, and `wallet_select_wallet()` (called by the
    home screen after an unlock when no wallet is active, and by
    selectWallet) clears `has_mnemonic` before loading the next seed. A
    zGetAccount in that window passed the wait; `wallet_with_seed()`
    then returned NO_MNEMONIC, which `send_wallet_error()` words as "no
    wallet on the device". The wait now requires `has_mnemonic`, and a
    NO_MNEMONIC on a board that has a wallet reads "the wallet is still
    opening on the device; try again" (still 0x0300). Sim test
    `test_zcash_waits_for_the_seed` (the fake vault now models the
    window) fails on the old wait.
  - Plausible, not changed: "derivation failed" is what
    `send_wallet_error()` says for any other error, including
    STORAGE_FAILED when `derive_lock_take()` waits 5 s in vain. Right
    after an unlock the home screen derives the fingerprint and pays for
    the BIP 39 seed (PBKDF2, 2048 rounds) under that lock, and the
    receive/wallet screens derive too; a zGetAccount arriving then waits
    on the lock, usually long enough, but the 5 s bound plus the
    Sinsemilla derivation on the worker can lose. A failed
    `zcash_worker_run()` reads the same, but it only fails without a
    worker task or after 120 s, so it is an unlikely source. With USB
    and BLE both up (Brave holding the
    USB port while Chrome paired over BLE), two hosts can drive the
    vault and the worker at once.
  - The 10 s deadline of `wait_for_wallet()` ends in NO_MNEMONIC when
    the vault is unlocked but no wallet gets selected (the home screen
    selects wallet 1 only when shown); rare, now worded as "still
    opening".
  - Host side, for all of these and for boards on older firmware:
    leek-device retries the first zGetAccount of a session once after
    1.5 s on 0x0300 (zecz, desktop, Android JNI). Not retried: other
    errors, later requests. The Chrome extension's own client is the
    other agent's.
- Tests: `cd host && cargo test` 80 passed, 6 ignored;
  `desktop/src-tauri` 31 passed, 3 ignored;
  `make -C firmware/sim test` 36 suites passed; desktop `tsc` clean.
- Left: on the board, a desktop sync of the account above should drop
  the 0.0102 TAZ and the shield prompt; the history should read "Sent to
  yourself" for faf76b13…; connect over Bluetooth right after unlocking
  a few times.

### 6.14 The Companion as a Zcash wallet; Android USB and release APK (2026-10-18)

The Android APK was tried on the board: Bluetooth worked, USB did not, the
screen still offered the EVM side, and the desktop window looked better than
the phone. Branch `companion-zcash-focus`.

- **Zcash-only by default.** `src/app-mode.ts`: `VITE_LEEK_APP_MODE` at
  build time, anything but `full` is the Zcash wallet. index.html ships
  `data-mode="zcash"` and marks the EVM elements `evm-only` (hidden by
  styles.css with `!important`, before any script runs): the Balances,
  Network (chain selector), Receive (with the token contract field), Send,
  Apps (mini apps), Connect a site (WalletConnect) and Activity sections,
  "Pair by QR" and its camera option, and the EVM "Testnets only" banner.
  `nav.ts` `sectionsFor()` keeps the EVM sections out of the accordion
  (test/nav.test.ts pins it); main.ts skips `getAddress`, balances, mini
  apps and the QR pairing. "Pair by QR" was the EVM air gap (scan the
  device's `ur:crypto-hdkey`, sign through `ur:eth-sign-request` /
  `ur:eth-signature`, src/qr-airgap.ts), not the board's pairing, which is
  the six-digit passkey over USB or BLE. The EVM code stays: `pnpm
  dev:full`, `pnpm build:full`, `VITE_LEEK_APP_MODE=full pnpm tauri ...`.
- **Home screen.** After unlocking, the Zcash wallet is the screen (its
  accordion header hidden, full width on a large window); the Device panel
  shrinks to the link and Disconnect; the passphrase panel moved below.
- **Phone layout** like the desktop's: buttons share a line instead of a
  stack of full-width ones, the Zcash tabs are one line scrolling inside
  itself, the network buttons and account on one line, pools as label and
  value, a one-line status bar, `viewport-fit=cover` with the existing
  safe-area padding, 16 px inputs (no zoom on focus). Checked with headless
  Chromium at 400×860 (2×, touch) and 1280×900, the wallet screens filled
  with stand-in data: no horizontal scroll at either width.
- **Passphrase = another wallet.** The board's account (`zGetAccount`) is
  read as soon as the home screen opens, and again after a passphrase,
  lock, network or account change, retrying once on 0x0300 (the vault
  still opening). Its seed fingerprint and account, with the network, key
  every wallet command (`WalletKey` in zwallet.rs):
  `wallets/<net>-<zfp16>-<account>`, `contacts-<net>-<zfp16>.json`; the
  server stays per network. A wallet of the first builds (`wallets/<net>`)
  moves to its keyed place when its own board account is read, and stays
  put for another seed. The passphrase panel says a passphrase opens a
  different, hidden Zcash wallet, kept apart by fingerprint, whose synced
  data stays on the device until forgotten.
- **USB on Android.** Not reproduced (no phone here). Likeliest cause: the
  board's Link was Bluetooth, and the board serves one link at a time, so
  USB enumerates but never answers. The UI hid that: failures went only to
  the collapsed log, and a handshake failure dropped the transport's own
  error. Now the Device panel shows the transport's words, then the advice.
  Checked and ruled out: the tested APK's merged manifest already had
  `usb.host` and a device filter (from the plugin's library manifest);
  `android-usb-serial` binds 303a:1001 as CDC-ACM through its interface
  fallback; DTR/RTS go 0,0 → 1,0 → 1,1, never through the chip's reset
  state. Fixed in transport-serial/src/android.rs: use the session path
  `open` returns, drain the plugin's RX hub (up to 64 KiB of unrequested
  bytes that `clear_buffer` does not touch) after opening and before each
  request, and keep the plugin's words in open errors.
  `scripts/android-usb-host.sh` is run after init (Espressif-only filter)
  and the release script refuses to build without it.
- **Release APK.** `pnpm android:release:arm64` (scripts/android-release.sh):
  release profile `opt-level = 3`, `lto = "fat"`, `codegen-units = 1`,
  `strip = true`; `panic` stays `unwind` (a panic in a command or a zecz
  worker must fail the call, not kill the app). Tauri leaves the APK
  unsigned; the script zipaligns and signs it with a local key
  (`~/.android/debug.keystore` by default, `LEEK_KEYSTORE*` to override),
  nothing committed. Sizes: debug arm64 APK 227 MB (library 220 MB);
  release arm64 APK 28.5 MB.
- Build note: a target directory copied from another checkout left
  tauri's build script unrun for the new `gen/`, so `TauriActivity.kt` was
  missing and Gradle failed in `compileUniversalDebugKotlin`;
  `cargo clean -p tauri --target aarch64-linux-android` fixed it.
- Tests: `pnpm typecheck` clean; `pnpm test` passes; `pnpm test:zdev`
  passes against zdev; `cargo test --lib` in src-tauri 27 passed, 1
  ignored (3 new: wallet keys, legacy adoption); `cargo check --target
  aarch64-linux-android` clean.
- Left, on the phone: USB with the board's Link on USB (and the red box's
  text if not); a passphrase on the board switching to its own wallet;
  the release APK installed over nothing (a different key than the debug
  one: uninstall first).

### 6.15 The Companion is Zcash only: EVM removed, renamed Zcash LeekWallet (2026-10-19)

- **Decision.** The firmware and everything around it are Zcash only, so
  the Companion's EVM side is removed, not hidden: chains, the RPC proxy,
  balances, tokens, EVM signing, Activity, mini apps, WalletConnect (with
  its bundled project id and relay), dapp connections, `ur:eth-*` QR
  air-gap, the blind-signing setting, the app-mode flag and their tests.
  Kept: USB and BLE transports, pairing and the encrypted session,
  unlock/lock, passphrase, wallet selection, `getStatus`/`getFeatures`
  and all of Zcash. The old MV3 browser extension (EVM) is gone; its icons
  moved to `chrome/extension/icons`.
- **Dependencies.** npm: `viem`, `@walletconnect/sign-client`,
  `zxing-wasm` dropped (the runtime is now `@noble/*` and
  `qrcode-generator`). Cargo: `reqwest`, `rustls`, `webpki-roots`, `url`,
  `webkit2gtk`, `libc`, dev `tokio` and the `rpc-proxy` feature dropped,
  with `rpc.rs` and `camera.rs` (no QR scanner, no camera permission). The
  CSP keeps only `self`, IPC and the GitHub hosts the flasher uses.
- **Diagnostics** now report only Zcash things: connection (transport,
  session, lock state), board (`getFeatures` firmware/model, then
  `zGetFeatures` over the session: backend, branch IDs, limits; the
  network setting is shown as not reported by the firmware), the
  passphrase XFP (fingerprint shown as hex, renamed "XFP" in the UI),
  Zcash network, lightwalletd server and last contact, synced height
  against the tip, last sync, and the device log. Never the passkey,
  passphrase, UFVK, addresses or data directory.
- **Name and id.** Shown as "Zcash LeekWallet" (productName, window title,
  Android launcher label). The identifier moves from
  `com.leekwallet.companion` to `io.leekwallet.zcash.companion`: it
  installs next to the old app, which can be uninstalled; the Zcash wallet
  syncs again from its birthday in the new one (nothing carries over).
  Desktop data directories move with the identifier too.
- **Firmware.** The "Blind sign" entry left the board's Settings (the
  setting, its warning screen and the `blindSigning` feature field stay,
  unreachable). `test_ui` checks the entry is absent. The EVM stages left
  `scripts/check.sh` and the sim Makefile.
- Tests: `pnpm typecheck` clean; `pnpm test` passes (core: cbor,
  device-state, framing, mock conformance with 26 regenerated vectors,
  mock device, session, zcash; flasher); `pnpm test:zdev` passes;
  `cargo test --lib` 16 passed, 1 ignored (the 11 RPC tests left with
  `rpc.rs`); `make -C firmware/sim test` and `pio run -e esp32s3cam` pass.
- Release arm64 APK: 24.6 MB (was 28.5 MB; library 21.9 MB).

### 10.6 "derivation failed": the Zcash worker never started (2026-10-19)
- Symptom since the first Android tests: now and then the board answered
  every zGetAccount with 0x0300 "derivation failed", over USB and
  Bluetooth, from every companion, and its own Receive screen said
  "(derivation failed)"; a restart sometimes cleared it.
- Logging added to each failure point of an account derivation showed
  "the Zcash worker did not run" on every request. The worker task's 24 KB
  stack came from the heap at boot, after Bluetooth and the camera had
  taken their internal RAM; on some boots (Link on Bluetooth) no block
  that large was left, xTaskCreate failed, and every derivation failed
  until the next restart. Shrinking it from 64 KB earlier had only made it
  rarer.
- Fix: xTaskCreateStatic with a .bss stack, so the linker reserves it
  (static RAM 60.6%). The logging stays.
- On the board: Receive and zGetAccount over Bluetooth work after several
  restarts with Link set to Bluetooth.

### 6.16 The Companion connects, sets up and syncs by itself; QR scanner for paying (2026-10-19)

Zodl's board flow is one guided path (pair, read the account and
birthday, create the wallet, sync); the Companion asked for four clicks
in two tabs. Branch `companion-auto-ux`.

- **Automatic.** After pairing the app asks for the PIN, reads the
  account (once more on 0x0300) and opens the wallet of its network,
  seed fingerprint and account, or creates it (`src/zcash-auto.ts`) at
  the board's birthday; a board without one gets the tip minus 100
  blocks, written back with `zSetAccountMeta`. The manual import shows
  only when that fails, with the reason and a retry.
- **Sync** (`src/sync-scheduler.ts`): on open, every 75 s in front,
  right after a send or a shield. Single flight; a request during a sync
  runs once after it. Held off between a proposal and its finish (the
  wallet runs one command per network: a sync starting after the board
  signed would make the finish fail). Paused when the webview is hidden.
  Home shows a dot and "Synced at block N", "Syncing X%" or the error
  with Retry.
- **Remembered board** (`src/board-memory.ts`): link and Bluetooth id and
  name only; the launcher's "Reconnect your board" goes to that id. The
  session is still a fresh handshake with passkey and ALLOW: no pairing
  key persists, by design, so there was none to reuse.
- **Home**: total, shielded (Orchard and Ironwood when both hold funds)
  and transparent, spendable and waiting, Shield whenever transparent
  funds exist.
- **Receive** (`src/receive-model.ts`): a fresh diversified address each
  time it opens, and "Board address (index 0)" to compare with the
  board's own screen; a new transparent address is the receiver of the
  next diversified UA, verified on the board through that UA (the board
  returns the P2PKH receiver at that index; the app compares). Rotation
  during a sync gets the wallet's busy refusal: the last address stays
  and a new one follows the sync.
- **Send**: Scan QR (`src/qr-scan.ts`, zxing-wasm 3.1.3 again, see the
  README for why not the barcode-scanner plugin), ZIP 321 parsed by
  `packages/core/src/zip321.ts`; the other network and multi-payment
  requests are refused before filling. A pasted `zcash:` link fills the
  form too. "Pay a contact" picker. CSP gains `'wasm-unsafe-eval'`; Linux
  gets the WebKitGTK camera grant back; `android-usb-host.sh` adds the
  CAMERA permission and an optional camera feature, and the manifest
  check requires them. No QR for pairing or signing.
- Checked end to end in headless Chromium (400×860) against zdev through
  a bridged fake backend: Connect → pairing → PIN → wallet created at the
  tip → synced, with no other click; Receive rotation, the board view
  and both verifications confirmed by zdev; contact pick; a mainnet
  request refused on testnet; a testnet request filled; Reconnect opens
  the existing wallet. Not committed (needs a browser from elsewhere).
- Tests: `pnpm typecheck` clean; `pnpm test` passes (new:
  `zip321.test.ts`, `companion-flow.test.ts`); `pnpm test:zdev` passes
  (new: the automatic set-up on Testnet and Mainnet boards and with a
  passphrase, rotated address and transparent receiver checked on the
  board); `cargo test --lib` 17 passed, 1 ignored (new: each network
  refuses the other's addresses).
- Release arm64 APK: 25.0 MB (was 24.6 MB; the decoder's 1.1 MB of
  WebAssembly, compressed).
- Left, on the phone: the camera prompt and a real scan; Reconnect over
  Bluetooth by id; sync pausing in the background.

### 6.17 Companion fixes from the tablet test: USB crash, Reconnect, dense QR codes, labels, wrong network (2026-10-20)

The release APK on a Samsung Galaxy Tab A7 (SM-T500, Android 16) with the
board: automatic connect, wallet and sync over Bluetooth, rotation, board
address, new transparent address, contacts, a testnet send, mainnet and a
passphrase all worked. Five things did not. Branch `companion-test-fixes`.

- **USB closed the app.** `tauri-plugin-serialplugin` 3.0.2 ran JNI
  `FindClass("app/tauri/serialplugin/UsbNative")` on the Rust thread that
  serves `ports`/`connect`. A thread attached from native code looks in
  the system class loader, which cannot see app classes, so it got
  `ClassNotFoundException`; the plugin left that exception pending, and
  ART turns a pending exception at thread detach into an uncaught one,
  which kills the process. Every USB attempt did this, whatever the
  board's Link setting. Upstream fixed both halves (3.0.7 caches the
  class in `nativeInit`, issue #42; 3.0.8 clears exceptions before
  detaching) and `android-usb-serial` 0.1.2 stops panics on a closed
  handle: now 3.0.9 / 0.1.2, diff read. Every USB failure is logged
  (`leek-usb:`) and returned with the logcat line to run
  (`adb logcat -s RustStdoutStderr SerialPlugin UsbFdBridge AndroidRuntime`);
  a worker panic is an error, and panics are logged as `leek-panic:`.
  Not seen on hardware yet: no logcat was taken, this is from the code.
- **Reconnect needed a board restart.** Closing the app (Back, or swiped
  away while Android kept the process) left the Bluetooth link open;
  the board does not advertise while connected. And the firmware closes
  its advertising window two minutes after a disconnect
  (`BLE_ADV_WINDOW_MS`, src/ble.c); then only a button press re-opens it.
  The app now closes the link (GATT disconnect, USB released) after 30 s
  in the background, on exit, and at start when the process still holds
  one (`src-tauri/src/link.rs`, `src/link-events.ts`); a request waiting
  on the board is never cut. Reconnect releases a stale link, scans 4 s,
  tries the remembered id directly (8 s), and otherwise says "The board
  is not advertising: it may still be connected to this or another app.
  Restart the board or wait ~30 s", and that a button press makes it
  advertise. Bluetooth commands run on their own task, so a blec panic
  is an error, and blec's disconnect (no deadline of its own) is bounded.
  For the firmware, later: keep advertising at a low duty cycle after
  the window instead of stopping, and ask for a short supervision
  timeout so a vanished phone frees the board in seconds.
- **Dense QR codes off a screen** (`src/qr-tuning.ts`, `src/qr-scan.ts`):
  the middle of the picture at full resolution every frame, then in
  rotation the whole frame, a contrast-stretched and darkened copy, and
  the GlobalHistogram binarizer; tryHarder and tryInvert as before.
  Brightness (exposure compensation, or the software darkening),
  Zoom when the camera has it, Switch camera, the camera remembered.
- **"undefined · date"** in Recent activity was the desktop fix's new
  `self` kind (a send to yourself). `src/tx-labels.ts` names it "Sent to
  yourself" with its fee, and any kind it does not know reads as its own
  words or "Transaction".
- **Wrong network.** 0x0528 now shows a notice on every tab with the
  board's words and *Switch the app to Mainnet/Testnet*, which switches
  and reads again (`src/network-guard.ts`). When `getStatus` carries
  `network` (the firmware agent is adding it) the app follows the board
  on connect and when it changes; without the field nothing changes.
- Tests: see the commit; NEAR swaps are not part of this (6.6).
