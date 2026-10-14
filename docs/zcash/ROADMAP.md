# Roadmap: DIY Zcash hardware wallet (ESP32-S3-N16R8)

There are no dates. Each step ends in a **done when** check. We do one step, debug it until the check passes, and only then move on. Background is in `ANALYSIS.md` (the why) and `PLAN.md` (architecture).

**Name:** not chosen yet; candidates are in §N at the end. This file says "the device".

**Hardware for the whole roadmap:**
- ESP32-S3-N16R8
- SSD1306 128×64 I2C display: SDA 8, SCL 9
- 4 buttons: UP 10, DOWN 5, CANCEL 6, ACCEPT 7 (LeekWallet `src/board.h:164-172`)
- microSD on 1-bit SDMMC: CMD 38, CLK 39, D0 40

**Not part of this roadmap:**
- eFuse burning. The device is experimental and private.
- Secure element. It stays an optional flag, as in LeekWallet.

### Rules for every step

1. Every firmware change goes through a fixed loop: **build → host simulator → QEMU (if it touches Xtensa/Rust) → real board**.
2. Every bug we fix gets a regression test in `sim/` or `vectors/`.
3. Each step gets a short entry in `docs/JOURNAL.md`: what was done, what broke, the numbers measured.
4. Nothing reaches the signing path without a refusal vector proving it rejects the bad case.

---

## Phase 0: Foundations

**0.1 Repository**
- `git init` in `zec hardware wallet/`.
- Layout as in `PLAN.md` §4: `firmware/`, `sim/`, `vectors/`, `host/`, `android/`, `docs/`.
- Fork LeekWallet into `firmware/` with its history kept, using a local clone.
- Done when: the tree exists and its first commit is the unmodified LeekWallet.

**0.2 Baseline build on your hardware**
- Build the *unmodified* LeekWallet `esp32s3` env and flash your board. Run `make -C sim` (the host tests).
- Done when: the board boots to the PIN screen, the buttons work, and the host tests pass. This proves the bench is good before we change anything.

**0.3 Toolchains**
- Install a pinned `espup` (Rust for Xtensa) and Espressif QEMU (`qemu-system-xtensa`).
- Record the exact versions in `docs/TOOLCHAIN.md`.
- Done when: `cargo +esp --version` works and `scripts/qemu.sh` boots LeekWallet.

**0.4 Rust-on-Xtensa spike** (the biggest unknown)
- Write a minimal `staticlib` using `pasta_curves`, `reddsa` and `orchard` (default-features=false, no_std+alloc). Link it into an IDF component.
- Run one RedPallas sign and one note-commitment (cmx) on QEMU and on the board. Print timings and peak heap.
- Done when: the numbers are in the journal.
- If the compiler crashes (esp-rs/rust#277), try older pinned toolchains. If none work, the C signer becomes primary and Rust stays host-only for now.

**0.5 Simulator upgrade**
- Extend LeekWallet `sim/`:
  - an SDL2 window showing the 128×64 framebuffer, scaled up;
  - keyboard → 4 buttons;
  - a transport on a pseudo-terminal (`/dev/pts/N`) and optionally TCP (for Android via `adb reverse`).
- Done when: the unmodified LeekWallet UI runs in the window, and the existing companion connects to the PTY as if it were a board.

**0.6 Test-vector generator** (`vectors/gen`, Rust on the host, librustzcash)
- **Key vectors:** mnemonic (with and without passphrase) → seed fingerprint, UFVK, unified address, transparent address.
- **PCZT vectors:** valid PCZTs, plus mutated ones that must be refused.
- Done when: JSON vectors are committed and cross-checked against `zcash_keys`.

---

## Phase 1: Device keys and storage

**1.1 Remove Ethereum**
- Delete `eth-decode`, `eth-tx*`, `eip712`, `eip4527*` and the ETH paths in `leek-wallet.c`, `protocol.c` and `ui.c`.
- Done when: it builds, the remaining sim tests pass, and the board boots.

**1.2 Platform changes**
- PSRAM on (settings from `sdkconfig.s3cam`), CPU at 240 MHz.
- Secrets allocated in internal RAM only.
- `PROTOCOL_MAX_FRAME` raised to 4096; uint64 support in `cbor.c`.
- A signing task with a large internal stack.
- Done when: a heap report shows PSRAM available, and a cbor uint64 test passes.

**1.3 `zsign` interface + key derivation in both backends**
- `zs_derive`: seed → Orchard-protocol keys + FVK, BIP44/133 transparent key, ZIP 32 seed fingerprint, UFVK, UA (F4Jumble + bech32m), t-address.
- Done when: the Rust and C backends both match every key vector.

**1.4 Storage modes**
- **Vault:** the LeekWallet vault plus per-account metadata (birthday height, account index, `use_qsk` flag, label).
- **Stateless:** LeekWallet's temp seed; nothing written to flash.
- **SD card:**
  - SDMMC 1-bit driver + FATFS;
  - a `.zkv` file holding `magic‖ver‖kdf params‖salt‖AES-256-GCM(seed‖birthday‖label)`;
  - unlocked by passphrase, never a PIN.
- Done when:
  - all three modes produce the same UFVK for the same seed;
  - the sim has a fake SD backend;
  - a power-cut test passes for vault writes;
  - a wrong passphrase fails cleanly.

**1.5 BIP39 passphrase handled correctly** (see §P)
- On-device entry uses LeekWallet's `text-entry`, restricted to printable ASCII so NFKD normalisation can't differ between implementations.
- After unlock, show the **seed fingerprint (short form)** so the user can see which wallet is open.
- Done when: the passphrase key vectors match, and different passphrases give different fingerprints.

**1.6 Receive UI**
- The UA paged across screens, never truncated.
- A t-address screen.
- A QR of the address (test whether a phone can read the UA QR on 128×64; if not, page the text only).
- Done when: a phone wallet scans or types the address and it matches the vector.

**1.7 Protocol: accounts**
- `zGetFeatures` (backends, branch IDs, storage mode), `zGetAccount` (UFVK + fingerprint + birthday), `zShowAddress`.
- Done when: they work through the PTY from a test script.

---

## Phase 2: Host core and CLI

**2.1 `zecz-host` crate (Rust)**
- Device transport (reuse LeekWallet `transport-serial` / `transport-ble`), framing, `LK` sync.
- **Session crypto ported from TypeScript to Rust**, checked against LeekWallet's conformance vectors.
- Done when: the CLI pairs with the sim and the board, and the session vectors pass.

**2.2 `zecz-cli` wallet** (testnet first)
- Import the account from the device (UFVK + birthday).
- Sync via lightwalletd gRPC using `zcash_client_backend` + `zcash_client_sqlite` (Zebra's built-in endpoint or Zaino).
- Commands: balance, receive.
- Done when: the CLI shows a testnet balance received to a device address.

---

## Phase 3: Rust signer (first signing path)

**3.1 PCZT intake**
- `zSignBegin{len, sha256}` / `zSignChunk` / `zSignFinish` into PSRAM.
- Parse PCZT v1 and v2.
- Done when: every vector loads, and oversized, truncated or wrong-hash uploads are refused.

**3.2 Checks** (order follows Keystone's `check.rs`)
- Reject `dummy_sk`.
- Spends: nullifier and rk against our own FVK.
- Outputs: recompute cmx, check `user_address` against the recipient, check cv via rcv.
- The ZIP 326 Orchard dummy output.
- Fee computed on the device.
- Branch-ID whitelist: NU6.3 `0x37A5165B`, NU7 `0x77190AD9`.
- Transparent inputs are our own path; SIGHASH_ALL only.
- Refuse Sapling spends.
- Done when: all mutated vectors are refused, each with a distinct reason code.

**3.3 Sighash**
- ZIP 244 (v5) and ZIP 229 (v6, the Ironwood leaf).
- Done when: the sighash matches the librustzcash vectors.

**3.4 Review UI**
- One page per output: pool, full address paged, amount, memo pages, a CHANGE tag.
- Fee; network name; a warning for transparent outputs; final ACCEPT.
- Done when: the screens are snapshot-tested in sim (LeekWallet `fake_oled` pattern).

**3.5 Sign and return signatures**
- Return `{pool, action_index, sig}` and `{input_index, sig}`. The host inserts them; external signatures are verified against rk.
- Done when: the host's combined transaction verifies.

**3.6 First real transaction**
- CLI builds the transaction, proves it in parallel, the device signs, then the CLI combines and broadcasts.
- First run through the sim, then through the board, on **testnet**.
- Done when: a testnet txid is confirmed. 🎉

---

## Phase 4: C signer and dual consensus

**4.1** Vendor `libzcash-ironwood-c` (pinned commit, MIT) + our own C postcard PCZT reader (bounds-checked).

**4.2** The C backend passes every key, PCZT and sighash vector.

**4.3 `BOTH` mode**
- The device signs only if both backends agree on the summary and the sighash.
- A disagreement gives a refusal screen.
- Done when: an injected disagreement (a test hook) is refused.

**4.4 Differential fuzzer in sim**
- The same random or mutated PCZTs go to both backends.
- A deterministic-nonce mode lets signatures be compared byte for byte.
- Done when: a long run finds no disagreements, or each one found has been fixed and added as a vector.

**4.5 Benchmarks on the board**
- Time per action, peak heap and binary size for Rust, C and BOTH.
- Done when: a table in the journal supports choosing the final backend.

---

## Phase 5: SD card airgap

**5.1 File formats**
- `unsigned.pczt` (raw PCZT) plus `unsigned.json` (hash, account fingerprint).
- `signed.zsig` (the signature list plus the PCZT hash it signs).
- The CLI writes and reads them.

**5.2 Device flow**
- Menu → SD → pick a file → the same checks and review as USB → sign → write `signed.zsig`.
- Refuse if the file changed between review and signing.
- Done when: a full testnet send uses only the SD card, with no USB data.

---

## Phase 6: Android (Zodl fork)

**6.1 Fork and baseline**
- Fork `zodl-android` (MIT).
- New `applicationId`, name and icons; Zodl trademarks removed.
- Done when: the rebranded APK builds and runs as a normal software wallet on testnet.

**6.2 Signature insertion**
- Read the Android SDK to see whether it can apply external signatures to a PCZT.
- If not, add a JNI function in a local SDK build (`settings.gradle.kts` L377 `includeBuild`), or switch the device to returning a full signed PCZT (Rust backend).
- Done when: we have a decision and a working proof of concept.

**6.3 `feature-hw` module**
- Copy `feature-ledger/`'s structure. Its flow is already wired: `createPcztFromProposal` → proof generation in parallel → signer → `submitTransaction(proven, signed)`.
- Transport: USB OTG serial, with the session ported to Kotlin and checked against the conformance vectors.
- Done when: the app pairs with the device (import UFVK + fingerprint via the Keystone account path).

**6.4** Send and receive from the app; the device signs. Done when: a testnet send is confirmed from the phone.

**6.5 Passphrase restore** (§P): add an optional passphrase field to the fork's software-wallet restore. Done when: the passphrase key vectors restore to the same addresses in the app.

**6.6 NEAR confidential swap**
- Add `"confidentiality"` to the 1Click quote.
- The device signs the deposit send and shows the deposit address (likely a t-address, with a warning).
- Done when: a mainnet swap of a small amount completes.

**6.7** BLE transport (LeekWallet GATT UUIDs + chunking).

---

## Phase 7: Desktop (Linux / Windows / macOS)

**7.1** A Tauri v2 app based on LeekWallet `app/`. The `zecz-host` crate does the backend work (sync, PCZT, proving, broadcast).

**7.2** Screens: pair, balance, receive (with "verify on device"), send, history. Done when: a testnet send works on all three operating systems.

**7.3** NEAR swap screen.

**7.4** Optional: SD airgap helper (write `unsigned.pczt` to the card, read `signed.zsig`).

---

## Phase 8: Chrome extension (thin)

**8.1** MV3 extension that catches `zcash:` ZIP 321 links and "Pay with ZEC" buttons and forwards them to the desktop app over **native messaging**. No keys, no wallet in the browser.

**8.2** A confirm popup with the amount and destination; the desktop app does the build/sign/send. Done when: payment to a demo web page works end to end.

---

## Phase 9: Hackathon submission (Wildcard / hardware)

**9.1** README: architecture diagram, a security model stated honestly (unburned, experimental, stateless/SD modes), and build and flash instructions.

**9.2** A demo script that runs fully on the simulator as a fallback and on the board as the main demo. Show:
- dual-signer agreement;
- a refused tampered PCZT;
- an SD airgap send;
- a confidential swap.

**9.3** Video, repository cleanup, licences (LeekWallet's own, MIT for the Zodl fork and libzcash-ironwood-c, MIT/Apache for librustzcash).

---

## Later (after the hackathon)

- QR airgap with the CAM board.
- ST7735 160×80 colour LCD.
- E-ink.
- ATECC608B PIN gate.
- Sapling spends.
- Choose the definitive signer backend.
- Follow-ups for NU7 and later upgrades.
- Burning on a dedicated unit, if it ever holds real value.

---

## §P: BIP39 passphrase, researched

**Zodl has no passphrase support.**
- There are zero matches for "passphrase" in the Kotlin code.
- The seed is always derived with an empty passphrase: `GetWalletSeedBytesUseCase.kt:15` is `Mnemonics.MnemonicCode(...).toSeed()`.
- The restore screen (`RestoreSeedVM.kt`) has no passphrase field.

**This does not affect hardware-wallet use.**
- With a hardware wallet, the phone never sees the seed. The device turns mnemonic + passphrase into a seed and exports only the **UFVK + seed fingerprint**. Zodl imports these through the Keystone account path (`CreateKeystoneAccountUseCase.kt` → `AccountDataSource.kt:192-206`).
- Each passphrase produces a different fingerprint, so each one appears in the app as a separate account automatically.
- Keystone does exactly this: its Cypherpunk firmware V-2.2.6 added Zcash passphrase wallets ([TradingView/Coindar](https://pl.tradingview.com/news/coindar%3A2373108e5094b%3A0-keystone-expands-zcash-support-with-passphrase-wallets); secondary source).

**The only gap is recovering into a *software* wallet if the device is lost.** Three fixes, all in this roadmap:
1. **Our Android fork adds a passphrase field** to restore (step 6.5). The bundled `kotlin-bip39` `toSeed()` takes an optional passphrase as a `CharArray` ([klibs.io](https://klibs.io/project/zcash/kotlin-bip39)), so the change is small.
2. **Our desktop app and CLI accept a passphrase** on restore (`zcash_keys` takes raw seed bytes).
3. **Any spare ESP32 flashed with this firmware** restores everything; this is the primary backup path.

**Interoperability rule:** BIP39 says to NFKD-normalise the passphrase; implementations that skip it restore a *different* wallet ([zec_wallet docs](https://pub.dev/documentation/zec_wallet/0.0.1/)). We **restrict passphrases to printable ASCII on every side** (device, Android, desktop), where NFKD changes nothing, and add a vector test that all three agree.

---

## §N: Name candidates (2 syllables, Yucatec Maya, easy in ES/FR/EN)

| Name | Meaning | Pronunciation (ES / FR / EN) | Notes |
|---|---|---|---|
| **Mukul** | "secret" | mu-KUL / mou-KOUL / moo-KOOL | Fits a privacy coin perfectly. "Mayan for secret" is attested only through the Nicaraguan **Mukul resort** marketing ([Oyster](https://oyster.com/articles/in-the-spotlight-mukul-luxury-resort-and-spa-nicaragua), [Resident](https://resident.com/index.php/2018/06/06/nicaraguas-emerald-gem/)); not yet confirmed in a Yucatec dictionary. A brand collision, but in a different industry. |
| **Kanan** | "guardian / to take care of" | ka-NAN, the same in all three | Confirmed in community use: Homún's cenote guardians call themselves *Kanan Ts'ono'ot* ([INAH](https://centroyucatan.inah.gob.mx/Administrador/images/5307871725558725.PDF)). A strong wallet meaning. Also a Star Wars character's first name. |
| Áak'ab / **Akab** | "night" | a-KAB | Confirmed (UADY-derived glossaries, [example](https://www.clubmitsubishiasx.com/faq/que-significa-noche-en-maya)). Fits "shielded = dark". ⚠️ Sounds like the slogan "ACAB" in EN/ES, so avoid it. |
| Taak'in / Takin | "money" | ta-KIN | Attested only in low-quality glossaries ([example](https://todosloshechos.es/como-se-dice-cuanto-es-en-maya)). ⚠️ Reads as "takin'" (taking) in English, and is also an animal. Avoid. |
| Balam | "jaguar" | ba-LAM | Easy and well known, but used by many brands; no privacy meaning. |

**Recommendation: Kanan** is the safest meaning that has been verified. **Mukul** has the best meaning for Zcash, but confirm it with a Yucatec speaker or the Bricker/Po'ot Yah *Hocabá* dictionary, or the Cordemex, before committing.

**Reusing "Leek":** prefer a separate name with a family tagline, e.g. "Kanan, a LeekWallet sibling". This chain has a different security posture (experimental, unburned), and keeping it separate protects Leek's reputation. LeekWallet's own roadmap already planned one firmware per chain (`leek-evm.bin`, `leek-btc.bin`), so a "Leek" family name is also legitimate. It's your call; the codebase doesn't care.
