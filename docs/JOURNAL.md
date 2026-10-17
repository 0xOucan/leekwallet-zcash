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
