# Roadmap: DIY Zcash hardware wallet (ESP32-S3-N16R8)

There are no dates. Each step ends in a **done when** check. We do one step, debug it until the check passes, and only then move on. Background is in `ANALYSIS.md` (the why) and `PLAN.md` (architecture).

**Name: LeekWallet Zcash** (decided). §N is kept for the record.

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
- `PROTOCOL_MAX_FRAME` raised to 4096. (uint64 in `cbor.c` turned out unnecessary: amounts travel inside the PCZT, which the signer parses itself, never as CBOR integers. The CBOR grammar stays as small as LeekWallet left it.)
- A signing task with a large internal stack.
- Done when: the generated sdkconfig shows PSRAM and 240 MHz, all envs build, and the sim suites pass.

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
- Status: SD and stateless done on PC; confirmed on the board on 2026-10-15: saved, read back on the PC by zecz, and loaded with the wrong and right passphrases. `.zkv` v1 is in FORMATS.md, with C (`src/zkv.c`) and Rust (`zecz zkv-new`/`zkv-open`) implementations that agree byte for byte on the golden file. The three modes give the same keys for all 48 vectors. A wrong passphrase loads nothing. A cut write leaves no seed file. The sim's fake SD is `sdcard.c` on a directory. Per-account metadata (birthday, `use_qsk`, label) is in `zcash-meta.c`. It persists only for a stored wallet without a passphrase, and stays in RAM otherwise. Its power-cut test passes.

**1.5 BIP39 passphrase handled correctly** (see §P)
- **Primary path: typed on the device**, as in LeekWallet. Also supported, as in LeekWallet's `setPassphrase`: entered on a companion and sent over the encrypted session. That is more convenient, but the passphrase then passes through the host. Both paths go through the same normalisation and the same ASCII rule.
- On-device entry uses LeekWallet's `text-entry`, restricted to printable ASCII so NFKD normalisation can't differ between implementations.
- After unlock, show the **seed fingerprint (short form)** so the user can see which wallet is open.
- Done when: the passphrase key vectors match, and different passphrases give different fingerprints.
- Status: done on PC. `ZFP xxxxxxxx` (first four bytes of the ZIP 32 seed fingerprint) on the home header after unlock and on the wallet screen; all 48 key vectors match; three passphrases give three fingerprints (JOURNAL).

**1.6 Receive UI**
- The UA paged across screens, never truncated.
- A t-address screen.
- A QR of the address (test whether a phone can read the UA QR on 128×64; if not, page the text only).
- Done when: a phone wallet scans or types the address and it matches the vector.
- Status: done on PC. UA pages, t-address page, both QR codes (lowercase, byte mode; UA v7 x1, t-address v3 x2), network under Settings. Every vector's QR scans back exactly with zbarimg from the framebuffer. The phone-off-the-panel check waits for the board.

**1.7 Protocol: accounts**
- `zGetFeatures` (backends, branch IDs, storage mode), `zGetAccount` (UFVK + fingerprint + birthday), `zShowAddress`.
- Done when: they work through the PTY from a test script.
- Status: done on PC. All three work through zdev's PTY in `tests/device_zdev.rs`. `zGetAccount` also returns the birthday, and `zSetAccountMeta` records it.

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
- Status: done on the board, 2026-10-15. The S3 CAM board signed testnet txid `1daaff6366fccbd6ea50f137a975a5013b2632083446b5220205ad16057b1ef0`: 0.01 TAZ with a memo, an Ironwood spend, reviewed on the SH1107 and accepted with the encoder.

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
- Status: done on the board, 2026-10-15. `zecz send` wrote `LEEKZ/unsigned.pczt` straight to the card. The board checked it, showed the review and wrote `LEEKZ/signed.zsig`. `zecz finish` read the card and broadcast testnet txid `30cdefb47286f2e3397dd13eaaba88fa772910ca41578a99baaf271adab993a4`.

---

## Where things stand (2026-10-16)

**Done and tested on the board (S3 CAM + SH1107 + EC11 + OV5640):**
- Phases 0-5: keys, the three storage modes (vault, stateless, SD `.zkv`), the C and Rust signers in agreement, review on the device, testnet sends over USB (`zecz`) and over the SD card with no link.
- Android (the Zodl fork, `android/` branch `leekwallet`): a LeekWallet account imported from the board, and testnet sends signed over **USB OTG**, **Bluetooth LE** and **QR**.

**Signing paths:**

| | CLI (`zecz`) | Android | Desktop | Chrome |
|---|---|---|---|---|
| USB | done | done | Phase 8 | Phase 9 |
| Bluetooth | - | done | Phase 8 | Phase 9 |
| QR | - | done (slow, see 7) | Phase 8 | Phase 9 |
| SD card | done | not planned (see 6.x) | Phase 8 | - |

**Recommendation to users:** USB or Bluetooth for everyday signing. QR or the SD card when the device must stay off every link.

**Order of work:**
1. Phase 7, the compact PCZT: small, and it halves the QR.
2. Finish Android for the demo (6.5-6.9).
3. Desktop (8).
4. Chrome (9), whose engine is the largest risk, so it starts with a spike.
5. The Zodl contribution (6.11), in parallel once Android is stable.
6. Firmware follow-ups (10).
7. Testing infrastructure (11), which runs alongside everything.

Sizes: S = a day or two, M = about a week, L = several weeks.

---

## Phase 6: Android - LeekWallet Zcash app, and a contribution to Zodl

One codebase for both: our app is the Zodl fork with our branding. The hardware support lives in `feature-leekwallet` plus a few ui-lib hooks, so the same module can be offered to Zodl without our branding.

**Done:**
- 6.1 Fork and rebrand.
- 6.2 Signature insertion (`host/leek-android`: Rust over JNI applies the `.zsig`).
- 6.3 Pairing and import.
- 6.4 A testnet send signed over USB.
- 6.7 Bluetooth LE.
- QR signing (`ur:leek-pczt` / `ur:leek-zsig`).

**6.5 Passphrase restore (S).** An optional BIP39 passphrase field on software-wallet restore (§P). Done when: the passphrase key vectors restore to the same addresses in the app.

**6.6 NEAR Intents from a LeekWallet account (M).**
- Zodl already ships 1Click swaps (`NearSwapDataSource`), and a LeekWallet account already goes through the Keystone proposal path that swaps use.
- First test a ZEC to USDC swap signed by the board. Then fix what breaks; the deposit is usually a transparent output, which the device already reviews with a warning.
- Then add the `confidentiality` option to the quote.
- Done when: a small mainnet swap completes, signed on the board.

**6.8 Compact PCZT in the app (S, after Phase 7).** Send the compact view over every link: half the QR frames, and faster USB and Bluetooth.

**6.9 Verify address on the device (S). Done (2026-10-16), rotating addresses included.** A Receive button that runs `zShowAddress`, so the user checks on the board's screen that the app's address is theirs.

**6.10 A proper account type (M).**
- Replace "Keystone-shaped account with key source `leekwallet`" with a `LeekWalletAccount` in the sealed hierarchy, plus the account-type switches it needs. About 80 sites.
- Needed before 6.11, and it removes the label bugs for good.

**6.11 Contribution to Zodl (M, then their timeline).**
- Open a discussion with Zodl first: hardware wallets are a policy decision for them.
- Prepare a branch `upstream/leekwallet-hw` from their `main`: the module, the ui-lib hooks, the native library build, tests, their detekt and ktlint rules, and no LeekWallet branding.
- If they decline, nothing is lost: our app carries the same code.

**6.12 Release (S).**
- A signed mainnet APK (our keystore), reproducible-build notes, and a GitHub release.
- F-Droid later, since the FOSS flavor exists.

**6.13 Staying current with Zodl (ongoing).**
- Rebase `leekwallet` on each Zodl release.
- The diff outside `feature-leekwallet` stays small on purpose.

Not planned: SD card in the app. Android's removable-storage rules and missing slots make it worse than QR there. If it is ever wanted, it becomes "export/import file" through the system picker.

---

## Phase 7: Compact PCZT (cross-cutting, S)

**Finding.** The Ironwood librustzcash already defines `SignerView::Compact` (the v2 PCZT encoding). It drops the note ciphertexts the signer can rebuild from recipient, value, rseed and memo, plus proofs and anchors.

- Measured on our real sends: Full view 2,791 B, Compact view **1,426 B (-49 %)**.
- The firmware already handles it. The C signer rebuilds `enc_ciphertext` from the memo (`zc_note_encrypt`). Through zdev in BOTH mode, both backends accepted a compact vector with the expected sighash (466827d9...).
- A security bonus: the device now encrypts the memo it shows.

**Status:** done. 7.1, 7.2 and 7.4 tested on the board over USB, Bluetooth and QR (tx `f7f6ad4b...`); 7.3 vectors on PC.

**7.1** Companions build the compact view.
- `zecz` gets a `--compact` option on `send`.
- `leek-android` gets `compactView(pczt)`, using the pczt redactor role as `compact_signer_view` does.

**7.2** The signed PCZT.
- The device's `.zsig` names the hash of the compact bytes. `PcztSignatures.apply` takes the compact bytes to check that hash, and applies the signatures to the Full view.
- The SDK combines that with the proven PCZT exactly as today.
- Test: compact -> device -> apply -> prove -> extract verifies (as `tests/device_zdev.rs`).

**7.3** Vectors.
- Add compact versions of the PCZT vectors.
- `test_zsign_diff` mutates them too.
- Firmware tests for a memo that does not re-encrypt to the carried commitment.
- Result: no commitment the compact view carries covers the memo, so a changed memo is a different transaction, not a refusal. The device shows it and signs a different sighash, and the host refuses those signatures. Output fields that disagree (epk, user_address, value balance) are refused.

**7.4** Make it the default on every link, after a board test over each. Done when: a QR signing takes about half the frames on the board.
- Result: 1,430 B; QR at 50-byte fragments went from ~56 frames to 28. Still slow, so the apps and docs present USB and Bluetooth (in a trusted place) as the everyday links, and QR and SD as the isolation options, not the efficient ones.

---

## Phase 8: Desktop - LeekWallet Zcash for Linux, Windows and macOS

**Architecture.**
- **Tauri v2**, starting from LeekWallet's `app/` already in `firmware/app`.
- **The backend is Rust and reuses what exists:**
  - `zecz` as a library: wallet database per profile (`zcash_client_sqlite`), lightwalletd sync, proposals, PCZT, proving with cached parameters, and broadcast.
  - `leek-device` (serial port).
  - `leek-transport-ble` (btleplug).
  - The webcam exposure code (`app/src-tauri/src/camera.rs`).
- **The frontend is TypeScript.** No protocol code in the webview.

**8.1 Skeleton (M).** `zecz` split into a library crate and a CLI. The Tauri app builds on Linux, Windows and macOS in CI, and opens and syncs a watch-only testnet account.
- Library split done: `host/zecz-core` (lib `zecz`) holds the wallet API (`zecz::wallet`: profile, init, sync with progress, tip, balances, addresses, propose a send, finish) and prints nothing; `host/zecz-cli` is the `zecz` binary over it, with the same commands and output.
- Tauri skeleton done: `desktop/` imports a UFVK as a watch-only account, syncs it with a progress bar, and shows balances per pool and the unified address. It builds and bundles on Linux (`tauri build --debug`), and its testnet sync test passes.
- CI written (`.github/workflows/desktop.yml`: Linux, Windows, macOS builds; host and backend tests). Left: its first green run once pushed.

**8.2 Software wallets (M).**
- Create or restore a 24-word seed with an optional passphrase.
- The seed is encrypted under the user's password (Argon2id, AES-GCM), and the key may also be held in the OS keychain.
- Several wallets and accounts.

**8.3 Hardware accounts (M). Done, tested on the board over USB and Bluetooth (2026-10-16).**
- Pair and import over USB and Bluetooth, sign over both.
- Address verification on the device.
- `.zkv` card creation as in `zecz zkv-new`.
- Done on PC (`desktop/src-tauri/src/hardware.rs`, `link.rs`): connect over
  USB serial or Bluetooth LE (leek-transport-ble adapted to leek-device's
  `FramePort`), pair with the passkey shown in the app, import with the
  birthday from the device or the user (recorded on the device), "Verify on
  device" on the receive card, and send: review (payments, change, fee),
  compact PCZT signed on the device, then prove and broadcast. Tested
  against `zdev` (pair, import, verify, sign; the signatures prove and
  extract). Left: the board over USB and Bluetooth, a broadcast send from
  the app, and `.zkv` card creation (moved to 8.4).

**8.4 Wallet screens (M).** Balance (Orchard, Ironwood, transparent), receive (UA, QR, "verify on device"), send with memo, history, contacts.
- Done on PC (2026-10-16), the board next: receive with a QR code,
  "New address" (fresh diversified addresses, Orchard + P2PKH within the
  transparent gap limit, then Orchard only) and the transparent address
  under a PUBLIC warning; "Shield transparent funds" (review, signed on
  the device like a send, zecz's refusal shown when the fee would take
  the balance); history from `v_transactions` / `v_tx_outputs` (date,
  kind, amount, fee, memo, txid with copy), refreshed after sync, send
  and shield; contacts as JSON in the app data dir, with a picker in the
  send form. zecz-core gained `next_address`, `check_address`,
  `history` and `store_transaction`. Tested offline (zecz-core and
  backend tests) and the history on testnet.
- Left: the board (a shielding signed on the device, a payment to a
  rotated address found by sync), and `.zkv` card creation (from 8.3).

**8.5 Air gap (M).**
- QR both ways with the webcam: the large-QR rules from Android, plus LeekWallet's manual exposure.
- SD card: write `LEEKZ/unsigned.pczt`, read `LEEKZ/signed.zsig`.

**8.6 NEAR Intents (M). Done on PC (2026-10-17); the mainnet swap with the board is next.**
- The 1Click API: tokens, quote (with `confidentiality`), the deposit send signed by software or the board, status polling, refunds.
- Done on PC (`desktop/src-tauri/src/near.rs`, the Swap screen): ZEC to
  any 1Click asset (the deposit built as a send and signed on the board)
  and any asset to ZEC (the deposit address shown to pay from elsewhere),
  refunds and payouts to a fresh Orchard-only address, quotes checked
  against the request, swaps kept in `swaps-main.json` and polled to
  SUCCESS / REFUNDED / FAILED. Testnet previews a price only. Confidential
  quotes need a 1Click API key (401 without). Tested offline against
  recorded answers and with a live dry quote.
- Left: the ZEC deposit address type (seen on the first real quote).
- Done when: a small mainnet swap completes from desktop.

**8.7 Packaging (M).**
- Linux: AppImage and .deb.
- Windows: MSI, code signing optional.
- macOS: .dmg, notarized. This needs an Apple developer account.
- Auto-update off by default.

---

## Phase 9: Chrome extension - a full wallet in the browser

Not the thin forwarder planned before: its own software wallets, the board over USB and Bluetooth, and NEAR Intents from the browser.

**Architecture.**
- **MV3.** The service worker only routes.
- **Device and long-running work in an offscreen document,** LeekWallet's proven pattern: a service worker has no `navigator.serial` and is evicted.
- **Device protocol:** the TypeScript `session.ts` and `framing.ts` from LeekWallet's `app/packages/core`, checked against the firmware's conformance vectors. Or `leek-device` compiled to WASM, decided in 9.1.
- **Transports:** Web Serial for USB, Web Bluetooth for BLE (from an extension page; it needs a user gesture), and a popup QR page.
- **Wallet engine:** our librustzcash rev in WASM, with threads, in a dedicated worker started by the offscreen document (9.1).

**9.1 Engine spike (M, the largest risk). Done (2026-10-16): see [CHROME-ENGINE.md](CHROME-ENGINE.md).**
- WebZjs is out: it is pinned to ChainSafe's NU6.1 librustzcash fork, with no Ironwood.
- Our own librustzcash rev, compiled to WASM (`chrome/engine-spike`), ran the whole flow in an extension worker: sync, propose, PCZT, sign, prove, extract. Broadcast is not tested yet.
  - It needs threads (single-threaded sync never finishes), so the engine runs in a dedicated worker with COOP/COEP.
  - It needs `zcash_client_sqlite` through a `libsqlite3-sys` stand-in over `sqlite-wasm-rs`.
  - It needs a gRPC-web endpoint: zec.rocks speaks only plain gRPC, ChainSafe hosts proxies.
- Numbers, 8 threads, laptop: prove 2 actions 9-10 s (single-threaded about 57 s, native 2.2 s), proving key 9-13 s, sync about 355 blocks/s, 145 MB memory, 4.3 MB gzip.
- Native messaging to `zecz` stays a fallback.

**9.2 Engine package and software wallets (L).**
- First, turn the spike into `chrome/engine`: the worker, a message API, sqlite on OPFS that persists, and a broadcast through the proxy.
  - **Engine and extension skeleton: done (2026-10-17).** `chrome/engine` (the message API in `chrome/README.md`), `chrome/extension` (offscreen document, engine worker, popup: network, import a viewing key, sync, balances, receive). Headless Chromium test: import, sync, stop and resume, and the wallet persisted on OPFS across an extension reload (mid-sync too) and a browser restart.
  - Pending: a broadcast through the proxy. The send path is in the API (propose, `.zsig`, prove, extract, broadcast) with an opt-in test that needs a funded testnet account.
- Then create or restore with a passphrase, encrypted with WebCrypto (Argon2id in WASM), auto-lock, several accounts.
- Done when: a testnet send from a software wallet, broadcast from the extension.

**9.3 The board (M).**
- Pair and sign over Web Serial and Web Bluetooth, and QR through the webcam.
- Done when: a testnet send signed by the board from the extension.

**9.4 Dapp interface (M).**
- Zcash has no EIP-1193. Offer `window.zcash`: connect, accounts, `sendTransaction(zip321)`, `signPczt`.
- Handle `zcash:` ZIP 321 links.
- Propose the interface publicly so other wallets can implement it.

**9.5 NEAR Intents from the browser (M).** A swap screen in the popup (1Click), and swaps requested by web pages through the provider.

**9.6 Store release (S).** The Chrome Web Store listing, permissions justified (serial, bluetooth, storage), a privacy policy.

---

## Phase 10: Firmware follow-ups

- **10.1 Transparent inputs (L): done; first shield signed on the board (2026-10-16).**
  - Sign P2PKH spends (ZIP 244 transparent digests; secp256k1 ECDSA) in both backends, with review pages.
  - Needed to shield funds received on a t-address, and for some swap refunds.
  - Done when: a testnet `zecz shield` signed on the board confirms, and the
    Android app applies a version 2 `.zsig`.
- **10.2 Measured timings on the board (S).** Roadmap 4.5: check and sign times per backend (C on the device; Rust only as a reference build).
- **10.3 Mainnet (S). Done (2026-10-17): a send and a shield signed on the board and accepted on mainnet, over USB with zecz.** Still to try on mainnet: the desktop app and the Android mainnet app. A small mainnet send over each link before any release.
  - Done on PC: mainnet PCZT vectors (a send and a shield, compact views,
    four cross-network refusals) through both signers, the sim, the soft
    signer and zdev; zShowAddress and the review pages on mainnet; zecz
    and zecz-core refuse a lightwalletd of the other chain; a watch-only
    mainnet sync of the public test account; the desktop app's network
    switch (saved, its own wallet per network, a red mainnet bar and an
    extra confirmation before signing).
  - Left: the first real mainnet receive, address check, send and shield
    on the board, with zecz and the desktop app, following
    `docs/zcash/MAINNET.md`.
- **10.4 Several accounts (S).** Account selection carried by every companion, not only account 0.
- **10.5 Bluetooth bonding (M).** Today every connection pairs with the six digits; optionally remember a host.
- **10.6 Security review (M).**
  - An honest threat model (no eFuses burned, experimental).
  - Fuzzing the PCZT readers on the board build.
  - Review of the new USB, BLE and QR paths.
- **10.7 Optional:** secure element, other screens (ST7735S, e-ink).

---

## Phase 11: Testing and debugging infrastructure (continuous)

- **11.1 CI (M).** GitHub Actions on every push:
  - firmware host suites and the esp32s3cam/pixie builds;
  - host crates;
  - the Android unit tests and the testnet APK;
  - the desktop builds on three operating systems;
  - the extension's unit tests.
- **11.2 Device in the loop (M).**
  - A script that flashes the board, pairs, imports and signs the vector PCZTs over USB and BLE, and checks the signatures.
  - The serial-relay log capture (`serial_tap.py`) made a tool.
- **11.3 End-to-end matrix (ongoing).**
  - Links (USB, BLE, QR, SD) times companions (CLI, Android, desktop, Chrome), each a testnet send checked on an explorer.
  - Results kept in `docs/JOURNAL.md`.
- **11.4 Testnet funds (S).** A faucet routine and a funded test account per companion; a small mainnet budget for 6.6, 8.6 and 10.3.

---

## Phase 12: Hackathon submission (Wildcard / hardware)

- **12.1 README:**
  - an architecture diagram (board, signers, companions);
  - a security model stated honestly;
  - build and flash instructions;
  - the wiring guide.
- **12.2 Demo:**
  1. Wallet created on the board.
  2. Import in the Android app.
  3. Sends over USB, Bluetooth and QR.
  4. SD airgap with `zecz`.
  5. A NEAR swap if 6.6 is done.

  The simulator is the fallback.
- **12.3 Video, repository cleanup, licences:**
  - LeekWallet's own;
  - MIT for the Zodl fork and libzcash-ironwood-c;
  - MIT/Apache for librustzcash.

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
