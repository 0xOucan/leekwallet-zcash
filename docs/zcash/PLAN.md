# Build plan: DIY Zcash hardware wallet on the ESP32-S3 (hackathon: Wildcard / hardware)

Companion to `ANALYSIS.md`. "LeekZ" is a working name; change it freely.

Decisions already taken:
- No eFuse burning.
- Proving happens on the phone/PC.
- Two signers (Rust + C), compared before choosing.
- Our own Android APK, forked from Zodl.
- Desktop companion for Linux/Win/Mac.
- The secure element stays optional.

---

## 1. Burning: what it protects, and what we do instead

Burning protects against exactly one threat: **someone who physically holds the device** (theft, a border check, an evil maid). Without flash encryption:
- `esptool.py read_flash` over the USB port copies the whole vault in seconds.
- The vault is AES-GCM under a PBKDF2 key derived from the PIN. A 4–8 digit PIN is cracked offline in minutes, and the 3-try wipe never runs because the attacker isn't using the device.
- Without secure boot, the attacker can flash a modified firmware that logs the PIN, give the device back, and wait.

Your point about DIY is correct for the *other* threats. You compile the firmware, the build is reproducible, and there is no vendor, factory or update server to compromise. Burning adds nothing there. It only matters for physical theft, and burning is irreversible on that chip, so skipping it for a dev/hackathon device is reasonable.

**What we do instead, so the "no burn" device is still honest:**
1. **Stateless mode is the headline mode** (LeekWallet's temp seed, the SeedSigner model).
   - The seed is entered (typed, or a SeedQR scanned on the CAM board) for each session.
   - Nothing is ever written to flash, so a dump reveals nothing.
2. **BIP39 passphrase.** It is never stored, so it is safe even if the vault is dumped.
3. **The stored-seed vault stays available as a convenience mode**, with a clear warning on the device: "unencrypted flash: PIN protects against casual use, not theft".
4. **Keys only in internal SRAM.**
   - PSRAM is unencrypted without flash encryption, so only *public/transaction* data lives there (PCZT buffers, Sinsemilla scratch).
   - Use `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` + `heap_caps_malloc(MALLOC_CAP_INTERNAL)` for secrets, as `sdkconfig.s3cam` already does.
5. **ATECC608B stays an optional build flag** (`LEEK_HAS_SE`), the same as in LeekWallet.

### 1b. Seed storage: three modes (the user picks at setup; all three coexist)

| Mode | Where the seed lives | Unlock | What a stolen device reveals |
|---|---|---|---|
| **Vault** | Internal flash NVS, LeekWallet vault unchanged (PBKDF2-SHA512 → AES-256-GCM, 30 slots, 3-try wipe) | PIN (+ optional passphrase) | Ciphertext; the PIN can be brute-forced offline (unburned) |
| **Stateless** | RAM only (LeekWallet temp seed); typed each session | Mnemonic entry | Nothing |
| **SD card** | `*.zkv` file on microSD: `magic‖ver‖kdf params‖salt‖AES-256-GCM(seed‖birthday‖label)` | **Passphrase** (a PIN is too weak; a card can be copied, so no wipe counter can exist) | Device: nothing. Card alone: ciphertext under a passphrase KDF. Both are needed. |

- **SD card pins:** 1-bit SDMMC on **GPIO 38 (CMD), 39 (CLK), 40 (D0)**, the same pins LeekWallet already reserved for the CAM board (`src/board.h:255-258`). No SD driver code exists yet: `esp_vfs_fat_sdmmc_mount` + FATFS.
- **SD airgap, bonus:** the same card carries `unsigned.pczt`, and the device writes `signed.zsig` (Coldcard/PSBT style). This gives a cable-free airgap that works on the 128×64 screen, where QR output is impractical.

### 1c. Is Zcash BIP39-compatible? Yes

- ZIP 32 derives Orchard/Ironwood (and Sapling) keys from the **64-byte BIP39 seed** (mnemonic + optional passphrase → PBKDF2-HMAC-SHA512). Transparent uses BIP44 `m/44'/133'/acct'`.
- Zodl, Keystone and Ledger all use BIP39. LeekWallet's `bip39.c` and `mnemonic-entry.c` work unchanged.
- **Use 24 words.** Zodl backups are 24 words; check whether Zodl's restore accepts 12.
- **Birthday height:** store the block height at creation with the seed (vault/SD) and show it. It isn't secret, but without it a restore must scan the chain from Sapling activation.
- **BIP39 passphrase:** it produces a completely different wallet. Zodl's restore does not take a passphrase (verify), so passphrase wallets can only be restored on this device or a wallet that supports it.
- **SLIP-39:** a 128-bit master secret gives a 16-byte seed, and `UnifiedSpendingKey::from_seed` requires ≥ 32 bytes (`zcash_keys/src/keys.rs` L274-296). It is also not importable elsewhere. Disable SLIP-39 for Zcash.
- **Don't reuse your EVM LeekWallet mnemonic** on this experimental, unburned device. The coin paths differ, but a dump of this device would expose those funds too.

---

## 2. Can we emulate everything before touching hardware? Yes, in three layers

| Layer | What runs | What it catches | Limits |
|---|---|---|---|
| **A. Native simulator** (`sim/`, daily driver) | Firmware C + both signers built for x86/ARM Linux/macOS. Display in an **SDL2 window at your real panel resolution**. Keyboard = buttons. Transport = **pseudo-terminal** (`/dev/pts/N`) or TCP socket. QR camera = image or webcam frames fed to quirc. | UI flows, protocol, PCZT checks, the companions talking to a "device" exactly as they would over USB | Host CPU, so no timing or Xtensa codegen |
| **B. Espressif QEMU** (already scripted in LeekWallet: `scripts/qemu.sh`) | The **real Xtensa binary** | Rust-on-Xtensa compiler bugs (esp-rs/rust#277); signer vectors run at boot and report over UART | PSRAM is broken (espressif/qemu#129), so only small vectors; no display or camera |
| **C. Wokwi** (optional) | ESP32-S3 + SPI TFT + buttons in a browser or VS Code; serial over RFC2217 | Peripheral wiring, the real display driver | Check PSRAM support and CLI licensing before relying on it |
| **D. Hardware** | Your S3-N16R8 + bigger screen (+ CAM board for airgap) | Real timings, BLE, camera | — |

Layer A is the important one.
- The companions connect to the simulator's PTY exactly as they would to `/dev/ttyACM0`.
- An Android emulator or a phone reaches the simulator's TCP port through `adb reverse`.
- The whole stack can be demoed end-to-end on a laptop, which is also the backup if the hardware misbehaves on stage.

This extends what LeekWallet already has: `sim/fake_oled.c`, `fake_usb.c`, `fake_input.c`. Swap the fake OLED for an SDL window and the fake USB pipe for a PTY or socket.

---

## 3. Two signers, Rust and C, behind one interface

### Interface (`components/zsign/zsign.h`, a C ABI both backends implement)

```c
typedef struct { /* parsed, VERIFIED view for the UI */
    uint32_t branch_id; uint8_t tx_version;
    uint8_t n_outputs; zs_output out[ZS_MAX_OUT]; /* pool, addr string, value, memo, is_change */
    uint64_t fee; uint8_t n_spends; uint64_t total_in;
    uint32_t warnings;               /* e.g. ZS_WARN_TRANSPARENT_OUT */
} zs_summary;

zs_err zs_load (zs_ctx*, const uint8_t *pczt, size_t len);           /* parse only */
zs_err zs_check(zs_ctx*, const zs_keys*, zs_summary *out);          /* recompute cmx/nf/rk/cv, fee, rules */
zs_err zs_sighash(zs_ctx*, uint8_t out[32]);
zs_err zs_sign (zs_ctx*, const zs_keys*, zs_sig_list *sigs);        /* only after user ACCEPT */
zs_err zs_derive(const uint8_t seed[64], uint32_t account, zs_keys*, zs_ufvk*);
```

### Backends

| Backend | Contents |
|---|---|
| **`zsign-rust`** | A `staticlib` crate built from upstream `pczt` (signer, no_std+alloc), `orchard` (default-features=false), `pasta_curves`, `reddsa`, `zip32`, `zcash_keys`, `f4jumble`, `blake3`. A Rust `#[global_allocator]` calls `heap_caps_malloc`. Its check list follows Keystone's `rust/apps/zcash/src/pczt/check.rs`. **Check Keystone's licence before copying; use it as a reference.** |
| **`zsign-c`** | `libzcash-ironwood-c` (MIT: Pallas, Sinsemilla, RedPallas, ZIP 32, UA, v6 sighash, ZIP 2005) plus **our own C postcard PCZT reader** (~1–2 k lines; postcard is varint + length-prefixed, easy to bound-check). |

### Build selection

A PlatformIO env or Kconfig `ZSIGN_BACKEND = RUST | C | BOTH`.

**`BOTH` = dual-implementation consensus.**
- Both backends parse and check the same PCZT independently.
- The device signs only if both agree on the summary and the sighash.
- If they disagree, it refuses and shows "implementations disagree".
- The flash space exists: the 4 MB app partition currently holds 1.1 MB.
- This is a genuine safety property (a bug in one implementation cannot silently move funds) and a strong hackathon story.

### Output is signatures only

- The device returns `{pool, action_index, sig[64]}` and transparent `{input_index, sig}`, the same shape as Keystone's `zcash-batch-sig-result`.
- The host inserts them into its PCZT; the Signer role verifies external signatures against `rk`.
- Benefits:
  - the C backend needs no PCZT *writer*;
  - the response fits one frame, or one or two QR frames on the device screen.

### Differential testing (host)

- Feed every vector and fuzz input to both backends in `sim/`.
- Compare verdicts, summaries and sighash.
- With an injected deterministic RedPallas nonce (`T`), compare the signatures byte for byte.
- This reuses LeekWallet's conformance-vector + `fuzz_transport.c` + `mutants.py` pattern.

### Choosing later

Measure on the S3:
- time per action
- peak heap
- binary size
- differential-fuzz findings
- how much work each protocol change costs (NU7 is the first test)

---

## 4. Repository layout

```
zec-hw/
  firmware/                  fork of LeekWallet (ETH removed, git history kept)
    src/                     ui, protocol, transport, vault, entropy… (reused)
    src/zec-*.c              account mgmt, review pages, protocol handlers
    components/zsign/        zsign.h + dispatcher + BOTH-mode comparator
    components/zsign-rust/   Cargo staticlib + CMakeLists (cargo +esp build)
    components/zsign-c/      libzcash-ironwood-c (vendored, pinned) + pczt-postcard.c
    components/display-*/    SSD1306 (kept) + your TFT driver behind oled-core's 6 funcs
  sim/                       native build: SDL display, PTY/TCP transport, both backends
  vectors/
    gen/                     Rust tool using librustzcash: builds PCZTs with known keys
    *.json                   expected sighash, summary, signature, and REFUSALS
  host/
    zecz-host/               Rust crate: device protocol + session, sync, PCZT build/prove/combine
    cli/                     zecz-cli (dev + scripted tests against sim/QEMU/board)
    desktop/                 Tauri v2 (from LeekWallet app/), Linux/Win/Mac
    extension/               Chromium MV3 (see §6)
  android/                   fork of zodl-android + feature-hw module
```

Test vectors come from librustzcash's own `pczt/tests/end_to_end.rs` patterns. Each valid vector gets **malicious mutations** that must all be refused:
- wrong cmx
- `user_address` not matching the recipient
- `dummy_sk` present
- unknown branch ID
- fee mismatch
- foreign FVK on a spend
- Sapling spend
- oversized memo

LeekWallet holds 60 vectors, 31 of them refusals; aim for the same ratio.

---

## 5. Firmware work items

1. **Strip ETH:** remove `eth-decode`, `eth-tx*`, `eip712`, `eip4527*`, the WalletConnect hooks and the `-DUSE_ETHEREUM/KECCAK` flags.
2. **Platform changes:**
   - CPU at 240 MHz.
   - PSRAM on (copy the `sdkconfig.s3cam` settings into the base config).
   - A dedicated signing task with a 32–64 KB internal stack.
   - The Sinsemilla 64 KB table as flash rodata.
3. **Display: SSD1306 128×64 I2C is primary for the hackathon.** It is LeekWallet's driver unchanged (I2C SDA 8 / SCL 9; buttons UP 10, DOWN 5, CANCEL 6, ACCEPT 7, per `board.h:164-172`).
   - **Optional second display:** the 0.96" module with `BLK CS DC RES SDA SCL` is a **ST7735S 160×80 IPS LCD** (BLK = backlight, which an OLED doesn't have; confirm from the listing).
     - Wiring: SCL→SCLK, SDA→MOSI on SPI2.
     - Suggested pins: MOSI 11, SCLK 12, CS 13, DC 14, RES 15, BLK 16 (free on the devkit; 35-37 are taken by octal PSRAM).
     - Driver behind the oled-core shim, like `oled-pixie.c`.
   - **E-ink: after the hackathon.** Slow refresh, and the wiring and controller are unknown.
4. **Keys:** ZIP 32 Orchard-protocol account key, BIP44 coin 133, seed fingerprint (ZIP 32), cached FVK, `use_qsk` (ZIP 2005; store the flag per account). Stateless mode first.
5. **Protocol** (CBOR, uint64 support added to `cbor.c`):
   - `zGetFeatures` (backends, branch IDs supported)
   - `zGetAccount` → UFVK + seed fingerprint
   - `zShowAddress` (UA + QR on the device)
   - `zSignBegin{len, sha256}` / `zSignChunk{seq, bytes}` / `zSignFinish`
   - The device shows the review and the response is the signature list.
   - Raise `PROTOCOL_MAX_FRAME` to 4096 (the spec already allows 4 KB); PCZT up to 512 KB in PSRAM.
6. **Review UI** (one page per item, never truncate; LeekWallet's rule):
   - each output: pool icon, full address over pages, amount in ZEC, memo pages, and a "CHANGE" tag when it belongs to us;
   - the fee;
   - a network/upgrade name from the branch ID;
   - a transparent-output warning;
   - final ACCEPT/CANCEL.
7. **Branch ID whitelist:** NU6.3 0x37A5165B, NU7 0x77190AD9. **Watch NU7 mainnet activation**; if it lands during the hackathon, both IDs must be live.
8. **Airgap (CAM board, stretch):** UR `zcash-accounts` out; `zcash-pczt` in; signature-list UR out.

---

## 6. Companions

### Android: fork zodl-android (MIT, Copyright 2026 Znewco Inc. d/b/a ZODL)

- **Rebranding:** the licence allows the fork. Change the app name, icons and `applicationId` (it is still `co.electriccoin.zcash`). Don't ship their trademarks.
- **`feature-ledger/` is your template.** It is already a *wired*, PCZT-based hardware-wallet module:
  1. `LedgerProposalRepository.kt` L220 `createPcztFromProposal`;
  2. L231-243 starts `addProofsToPczt` in parallel;
  3. the device signs;
  4. L583 `submitTransaction(proven, signed)`.
- Create **`feature-hw`** by copying its structure. Replace `LedgerSigningDataSource` (which calls the SDK's Ledger-specific `signPcztWithLedger`) with our transport:
  - **USB OTG serial** (`usb-serial-for-android`)
  - **BLE** (Android GATT, using LeekWallet's service UUIDs and chunking)
  - **QR** (reuse the Keystone screens)
- **Account import:** reuse the Keystone path (`CreateKeystoneAccountUseCase`: UFVK + seed fingerprint).
- **Applying signatures:** check whether the Android SDK (`com.zodl.android:zcash-android-sdk`, Rust backend) exposes "apply external signatures to a PCZT". If not:
  - the SDK can be built locally (`settings.gradle.kts` L377 `includeBuild` substitution), so we add one JNI function;
  - or the device returns the full signed PCZT from the Rust backend.
  - **Verify this early (day 1–2); it decides the device output format.**
- **Session crypto** (X25519 + ChaCha20-Poly1305) has to be ported to Kotlin. LeekWallet's lives in TypeScript (`app/packages/core`); use BouncyCastle or Tink plus LeekWallet's conformance vectors.
- **NEAR confidential swaps are already in Zodl** (`NearApiProvider.kt`, `createExactInputSwapProposal` exists for hardware accounts). Add `"confidentiality"` to the quote request. **The device only sees a normal send to a deposit address.**

### Desktop (Linux/Win/Mac): Tauri v2, starting from LeekWallet's `app/`

- Reuse the shell, `transport-serial` (Rust), `transport-ble` (btleplug), the flasher and CI.
- **Rust backend `zecz-host`** (in `src-tauri`):
  - `zcash_client_backend` + `zcash_client_sqlite` with `lightwalletd-tonic` sync against Zebra's built-in lightwalletd endpoint or Zaino (public servers exist, e.g. zec.rocks: verify);
  - proposal, PCZT, prove (native, fast), combine, broadcast;
  - NEAR 1Click.
- This is the biggest companion job: a wallet backend, not just a bridge.
- **Hackathon cut:** sync + balance + receive + send + swap.

### Chrome extension: makes sense in one specific form

- The LeekWallet extension exists to be an EIP-1193 provider for dapps. **Zcash has no dapp-provider ecosystem**, so a direct port makes no sense.
- Two forms that do:
  1. **Thin extension + native messaging to the desktop app (recommended).**
     - The extension catches `zcash:` ZIP 321 payment links and "Pay with ZEC" buttons on web shops and opens a confirm popup.
     - The desktop companion builds, proves and asks the device to sign.
     - About 1–2 days of work, no WASM wallet, and it fits a "shielded checkout" story.
  2. **Standalone browser wallet:** librustzcash compiled to WASM (ChainSafe WebZjs-style), gRPC-web to a lightwalletd proxy, Halo2 proving in the browser, Web Serial to the device.
     - Doable, but Ironwood/v6 support in the WASM stack is unverified and it is the riskiest piece.
     - Post-hackathon.

---

## 7a. Schedule

Superseded by `ROADMAP.md`: step by step with done-when checks, no dates.

## 7. Milestones (hackathon cut line marked)

| # | Milestone | Done when |
|---|---|---|
| M0 | **Toolchain spike**: `espup` toolchain pinned; `zsign-rust` builds for `xtensa-esp32s3-espidf` and links into IDF; runs one RedPallas sign + one cmx on the board and in QEMU | Timings + heap printed; no compiler crash |
| M1 | **Simulator**: native firmware with SDL display, PTY transport, stateless seed entry, UFVK export, UA display | `zecz-cli` reads the UFVK from the sim; matches `zcash_keys` for ZIP 32 vectors |
| M2 | **Vectors**: Rust generator for valid + mutated PCZTs (v6, Ironwood, transparent in/out, Orchard turnstile dummy) | JSON vectors committed; Rust backend passes all in sim |
| M3 | **C backend** + postcard reader; BOTH mode; differential fuzzer | C passes all vectors; fuzzer runs N hours with no disagreement |
| M4 | **CLI end-to-end on testnet**: `zecz-cli` syncs, builds, proves, signs via the sim, then via the board | A testnet tx confirmed, signed by the device |
| M5 | **Android fork**: `feature-hw` over USB OTG + BLE; pair, send, swap | A testnet send from the phone; a mainnet **confidential swap** with a small amount |
| M6 | **Board polish**: TFT UI, real timings, demo script, video | — |
| — | **HACKATHON CUT** | |
| M7 | Desktop Tauri companion | Send/receive on Win/Mac/Linux |
| M8 | Extension (native messaging, ZIP 321 checkout) | Pay a demo web shop |
| M9 | Airgap QR (CAM board) | Pair/sign by camera |
| M10 | Choose the definitive backend; optional ATECC608B; optional burn on a *dedicated* unit | — |

If time is short, M7 can be replaced by "the CLI is the desktop companion" for the demo.

### Wildcard pitch

A shielded-ZEC hardware wallet anyone can build from a ~$10 ESP32-S3:
- it **recomputes every note commitment** before signing;
- **two independent implementations (Rust and C) must agree** before it signs;
- it runs **stateless** (nothing on flash to steal);
- it does a **confidential cross-chain swap** through NEAR Intents from our own Android app.

---

## 8. Main risks, and the early check for each

| Risk | Check |
|---|---|
| esp-rs Xtensa compiler crash on heavy generic crates | M0, day 1. If blocked, try an older pinned toolchain; worst case, ship C-only for the demo. |
| Android SDK cannot apply external signatures | Day 1–2 code read of the SDK; fallback: device returns the full signed PCZT (Rust backend). |
| Signing too slow on the S3 (many actions) | M0 timings; FVK cached; limit actions per tx in the UI. |
| NU7 mainnet activation during the window | Whitelist both branch IDs; test expiry-height handling. |
| 1Click ZEC deposit addresses are transparent | The device must render t-addresses + a warning; verify with one live quote. |
