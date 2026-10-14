# Zcash hardware wallet on the ESP32-S3-N16R8: analysis

Date: 2026-10-14.

Sources:
- `../LeekWallet` (C, ESP-IDF 5.5)
- `../zec references/` (librustzcash, zebra, zodl-android, zodl-project)
- The pages you provided, plus current web sources. URLs are inline below.

---

## 0. What has changed since most Zcash documentation was written

The readthedocs pages you linked (basics, zcashd, protocol, zig) are years out of date. They describe Canopy-era t/z addresses. The ground truth today, confirmed in the local `librustzcash` and `zebra` checkouts and on zips.z.cash:

| Fact | Consequence for the device |
|---|---|
| **The Orchard circuit soundness bug** (found May 2026). Orchard was frozen by soft fork; **NU6.2** (h 3,364,600) shipped a fixed circuit. | Host provers need the current circuit. The signer is unaffected. |
| **NU6.3 "Ironwood"** (ZIP 258), mainnet h **3,428,143**, branch ID **0x37A5165B**. A new Orchard-protocol pool holds ~83% of shielded ZEC. Orchard is a "turnstile": funds can leave but not enter. | The device **must sign Ironwood actions**. Orchard is needed only for migrating and withdrawing. |
| **Transaction v6** (ZIP 229, group ID 0xD884B698): a 5-leaf txid digest with an Ironwood leaf, and anchors moved into the auth data. | Implement the v6 sighash (`zcash_primitives/src/transaction/sighash_v6.rs`) as well as v5. |
| **ZIP 2005**: Ironwood notes use lead byte 0x03, a new ψ/rcm derivation, and the optional `use_qsk` key, with qk derived via **BLAKE3**. Hardware wallets are told to keep qsk on the device. | Add BLAKE3. Make qsk a device secret. |
| **ZIP 326**: every Orchard spend gets a fabricated 0-value output back to the same address, with a *random* enc_ciphertext. | The signer must accept this dummy while still checking its cmx (`pczt/src/roles/signer/mod.rs` L16-33). |
| **PCZT = ZIP 374** (Draft). v1 (Keystone-era) and v2 (optional bundles, Ironwood) encodings. Magic `PCZT` + u32 LE + postcard. | This is the wire format. Do not invent a new one. |
| **NU7** (ZIP 259): testnet h 4,465,026, branch ID 0x77190AD9, mainnet height still being set. 25 s blocks, fee burn, NSM. **No ZSAs, no new tx format.** | Add the branch ID to the whitelist and keep the expiry logic flexible. |
| **zcashd stopped itself on 2026-07-18** (h 3,417,100) and its repo is archived. The successors are Zebra (with a built-in lightwalletd gRPC server, `zebra-rpc/src/lightwalletd.rs`), Zaino, and Zallet (beta). | Host sync goes through Zebra or Zaino, never zcashd. |
| **Zashi became Zodl** (ECC → ZODL, Feb 2026). Zodl already signs through Keystone over QR using PCZT, and does NEAR 1Click swaps. | A ready-made host app exists if the device speaks Keystone's UR profile. |

---

## 1. The core architectural fact: the device signs, the host proves

Proving cannot happen on an ESP32-S3:
- `sapling-spend.params` alone is 47.9 MB (`zcash_proofs/src/lib.rs` L41-57), about 3× the flash and 6× the PSRAM.
- The Orchard/Ironwood Halo2 proving key is built in RAM, and "proving requires `std` in practice".

Every shipping hardware wallet (Keystone, Ledger) splits the work the same way:

```
HOST (untrusted)                                  DEVICE (trusted)
sync (Zebra/Zaino), notes, witnesses
Creator → Constructor → IO Finalizer → Updater
Redactor (SignerView::Full) ──── PCZT ──────────▶ parse
Prover (Halo2) runs IN PARALLEL                   CHECK: recompute cmx, nf, rk, cv; own-address,
                                                         dummy-output rules; fee; branch ID whitelist
                                                  DISPLAY: every output, fee, change marked "mine"
                                                  SIGN: ZIP 244/229 sighash → RedPallas (Ironwood/
                                                         Orchard), ECDSA (transparent)
Combiner(proven, signed) ◀── signed PCZT ──────── 
Spend Finalizer → Tx Extractor → broadcast
```

Zodl does exactly this. It starts `addProofsToPczt` the moment it hands the PCZT to Keystone (`zodl-android/.../KeystoneProposalRepository.kt` L220-234). It then combines the result with the signed copy (`pczt/tests/end_to_end.rs` L627-672 shows the same pattern).

**This matches LeekWallet's philosophy.** The device decodes everything itself, and anything it cannot recompute it refuses. A PCZT contains enough plaintext (recipient, value, rseed, rcv, alpha) for the device to *re-derive* every commitment it signs over. That is stronger than the EVM case, where it had to trust a decode table.

### What the device must hold and do

- **Secrets:**
  - the BIP39 seed (the LeekWallet vault, unchanged);
  - ZIP 32 Orchard-protocol `sk` → `ask`, `nk`, `rivk` (plus `qsk` if `use_qsk`);
  - the BIP44 `m/44'/133'/acct'` secp256k1 key.
- **Exports:**
  - the UFVK (Orchard FVK = ak‖nk‖rivk, plus transparent xpub; add the Sapling DFVK if you support Sapling);
  - the ZIP 32 seed fingerprint.
- **Must verify before signing** (ZIP 374 signer duties, the Keystone `check.rs` set):
  - reject any `dummy_sk`;
  - each spend: `verify_nullifier`, and rk = ak + [α]G against **its own** FVK;
  - each output: recompute cmx (Sinsemilla NoteCommit), check that the `user_address` UA contains the recipient, and check cv via rcv;
  - ZIP 326 dummy outputs;
  - fee = Σin − Σout computed on the device;
  - branch ID and tx version against a whitelist;
  - transparent inputs derive from its own path, with SIGHASH_ALL only.
- **Sapling spends: skip in v1.**
  - Keystone skips them too.
  - They need all of Jubjub/RedJubjub plus the 47 MB-params proof on the host.
  - Only ~10% of shielded value is in Sapling. Users migrate it with a software wallet.
  - Sapling *outputs* can be refused or displayed.

---

## 2. What LeekWallet gives you (roughly 60% of the firmware)

The LeekWallet source is 25.9k lines in `src/`, plus 3.6k in `components/leek-wallet` and 28.7k in trezor-crypto.

| Reuse as-is | Adapt | Replace |
|---|---|---|
| vault (PBKDF2-SHA512 → AES-256-GCM, 30 seeds, atomic PIN change)<br>PIN with power-cut-safe 3-try wipe<br>entropy gate + dice<br>temp seed<br>SLIP-39<br>device wipe<br>session (X25519 commit-reveal + ChaCha20-Poly1305)<br>BLE/USB framing<br>button/OLED/screen-table UI<br>text/mnemonic entry<br>BC-UR fountain encoder/decoder<br>quirc + OV5640 camera<br>sim/ host test harness, fuzzers, mutants, conformance vectors<br>reproducible builds | `leek-wallet.c` (derivation: coin 60 → ZIP 32 + coin 133)<br>`protocol.c` (method table + streaming PCZT upload)<br>`cbor.c` (**add uint64**: zatoshi amounts)<br>`airgap.c` (UR type gate, buffer sizes)<br>`ui.c` sign pages (generic "N wrapped pages")<br>`ble-chunk`/`PROTOCOL_MAX_FRAME` | `eth-decode.c`, `eth-tx*.c`, `eip712.c`, `eip4527*.c` (~3.6k lines) → Zcash PCZT check/parse/sign + Keystone-style UR schemas |

### LeekWallet limits that matter for Zcash

1. **Payload size.** `PROTOCOL_MAX_FRAME 1024` (`protocol.h:37`). On the QR path the static buffers are `airgap.c:24-25` and `UR_MAX_PARTS 128`. A real PCZT is 2.7 KB for a one-action Keystone vector (`pczt/tests/firmware_compat.rs`), and multi-action ones run to tens of KB. You need chunked `pcztBegin/Append/Finish` commands into a PSRAM buffer.
2. **PSRAM is disabled on purpose** (`sdkconfig.defaults` ~L92-111). PSRAM is only encrypted once flash encryption is burned. For Zcash you need it: PCZT buffers, Rust `alloc`, Sinsemilla scratch. This **ties PSRAM to finally doing T11** (flash encryption + secure boot), which LeekWallet has only rehearsed in QEMU (`docs/BURN-PROCEDURE.md`). Today the open `AUDIT.md` S1 applies just as much here: the vault can be read off flash.
3. **The display.** 128×64 is 8×21 characters.
   - A Ironwood/Orchard UA (~140-220 chars) needs 2 pages.
   - A 512-byte memo needs about 5 pages.
   - Returning a **full signed PCZT** by QR (what Zodl's `parsePczt` expects) from a 0.96" panel means dozens of 0.17 mm-pixel frames.
   - **Strong recommendation:** use a 240×240 or 320×240 colour panel for the Zcash build. The `oled-pixie.c` shim already proves a panel swap costs about 6 functions.
4. **CPU at 160 MHz.** Raise it to 240 MHz; curve maths is the bottleneck.
5. **The crypto stack has none of what Zcash needs except BLAKE2b/2s with personalization, bech32m, ChaCha20-Poly1305 and secp256k1.** Missing:
   - Pallas/Vesta fields and curves
   - Sinsemilla (+ 64 KB point table)
   - Poseidon
   - RedPallas
   - ZIP 32 Orchard derivation
   - F4Jumble
   - FF1-AES (diversifiers)
   - BLAKE3
   - the v5/v6 sighash
   - a PCZT postcard parser

---

## 3. The main decision: how to get Zcash crypto onto Xtensa

| Option | Pros | Cons |
|---|---|---|
| **A. Rust staticlib**: upstream `pczt` (signer, no_std+alloc), `orchard` (default-features=false), `pasta_curves`, `reddsa`, `zcash_keys`, `zip32`, `f4jumble`, linked into the existing ESP-IDF C firmware as a component | The same code Keystone ships (`rust/zcash_vendor`), and Ledger's official app is Rust too. **Protocol churn comes for free.** This year alone brought NU6.2 → NU6.3 → v6 → PCZT v2 → ZIP 2005 → NU7. The verification APIs (`verify_nullifier`, `verify_rk`, `verify_note_commitment`) already exist. | Xtensa is still **not in upstream LLVM/rustc**; you need the esp-rs fork via `espup` (v1.99.0.0). There is an open codegen ICE since 1.94 (esp-rs/rust#277). Binary size and heap use are unknown. |
| **B. C**: `wh00hw/libzcash-ironwood-c` (MIT, C11, no malloc, trezor-crypto lineage, already has the v6 sighash, ZIP 2005, cmx + enc_ciphertext recompute, and an ESP32-S2 reference firmware) | Same language and style as LeekWallet; deterministic memory; no exotic toolchain. | A single-author proof of concept, **unaudited**. You inherit every future protocol change by hand. It has no PCZT parser; its host SDK sends sighash+alpha, which is effectively blind signing. |
| C. Hybrid: Rust for PCZT parse + verify, C for the hot path | | Two sets of maths to keep in agreement; complexity. |

**Recommendation: A, with B as a fallback and a cross-check.**
- Week 1 is a spike: build `pczt` + `orchard` no_std for `xtensa-esp32s3-espidf`, link it into an IDF project, and measure:
  - one RedPallas signature;
  - one NoteCommit (cmx);
  - UFVK derivation (commit_ivk);
  - peak heap.
- If the toolchain blocks you, switch to B.
- Keep B's test vectors in either case. They drop into LeekWallet's `sim/` conformance-vector pattern (C device ↔ Rust host).

### Performance expectations (unverified; measure them)

| Platform | Measurement |
|---|---|
| Flipper Zero (Cortex-M4 at 64 MHz) | ~5 s per RedPallas signature; >12 min for Sinsemilla without a table |
| Hanh's Ledger app | 1:30 for Orchard key derivation |
| ztrezor cost model | scalar mult ≈ 5,334 field muls; NoteCommit = 109 Sinsemilla blocks + 1 mult; commit_ivk = 51 blocks + 1 mult |
| ESP32-S3 at 240 MHz (expected) | Several times faster than the Flipper. Roughly ≤1 s per signature, a few seconds per action including cmx/nf/rk checks. Derive the FVK **once** and cache it encrypted in the vault. |

- Put the **64 KB Sinsemilla table in flash rodata**; 12 MB of the 16 MB is unallocated.
- The S3 MPI (RSA) peripheral *might* speed up 255-bit Montgomery multiplication. Ledger does the same with its `cx` bignum hardware (`app-zcash/ledger_zcash_crypto/src/redpallas.rs`). This is an optimisation for later.

---

## 4. Host side and transport: three paths, in this order

1. **USB serial + a Rust CLI built on librustzcash** (development path).
   - Reuse `app/transport-serial` (Rust, already handles the `LK` sync).
   - The host builds the PCZT with `zcash_client_backend`, using Zebra/Zaino on **testnet**. It applies `redact_pczt_for_signer` (`SignerView::Full`), proves in parallel, combines, extracts and broadcasts.
   - This is the fastest edit-flash-test loop.
2. **Keystone-compatible QR** (gets Zodl as the host app, with zero host work).
   - UR types (spec: `keystone3-firmware/docs/protocols/ur_registrys/zcash.md`):
     - `ur:zcash-accounts` {seed-fingerprint, [zcash-ufvk{ufvk, index, name}]}
     - `ur:zcash-pczt` {data}
     - later `zcash-sign-batch` / `zcash-batch-sig-result` for migration (96 actions per round in `zcash_pool_migration/src/signing_rounds.rs`).
   - This needs the CAM board (OV5640 + PSRAM, already supported) and, practically, a bigger screen for the return QR.
   - **Caveat:** Zodl rejects a signed PCZT unless `global.proprietary["keystone:fw_version"]` ≥ 3.0.1 (`KeystoneFirmwareVersion.kt`). Setting that stamp makes your device pose as Keystone firmware. Decide that consciously, and ideally ask ZODL for a neutral signer-version key.
3. **Tauri companion** (the LeekWallet `app/` rewritten for Zcash).
   - `src-tauri` hosts librustzcash: sync, prover, NEAR 1Click.
   - The large part is the wallet backend (sync, note storage). Only build this after 1 and 2 work.

### NEAR Intents / confidential swaps

- From the device's point of view, a swap is an **ordinary send**.
- The host (as Zodl does in `NearApiProvider.kt`):
  1. Calls `POST 1click.chaindefuser.com/v0/quote` with `"confidentiality": "basic"|"advanced"`.
  2. Gets a `depositAddress`.
  3. Sends ZEC to it.
  4. Calls `deposit/submit` with the txid.
  5. Polls `status`.
- ZEC is `nep141:zec.omft.near`, 8 decimals.
- The deposit address is likely **transparent t1** (unverified). The device must therefore render and allow shielded→transparent outputs.
- The device **cannot verify the swap terms** (rate, destination chain). Apply the LeekWallet "two gates" rule:
  - the host shows the quote, labelled *untrusted*;
  - the device shows only what it can prove: amount, fee, and the t-address it is paying.
  - Optionally the host passes the quote hash and the device displays it as "host-claimed".

---

## 5. Security posture (be honest about it, as LeekWallet's audits are)

- **ESP32-S3 has no secure element.** Glitching and side channels are not covered, and the 3-try PIN counter lives in NVS (reflashing resets it).
  - `wh00hw`'s ESP32 README says outright that this is "not a hardware-wallet threat model".
  - LeekWallet's ATECC608B research (`docs/RESEARCH-SECURE-ELEMENT.md`) applies directly.
- **Flash encryption + Secure Boot v2 + JTAG disable** are what keep the encrypted vault and the PSRAM contents from being dumped. They are mandatory before holding real funds.
- **Constant-time signing:** the audit of Hanh's Ledger app found non-constant-time RedPallas. Use the pasta_curves/reddsa implementations and avoid secret-dependent branches in any C port.
- **Audits to borrow from:**
  - Least Authority on Keystone's Zcash app (2025, no critical findings);
  - defuse.ca on Hanh's Ledger app;
  - LeekWallet's own `AUDIT-*.md` (the open items S1, entropy F-3..F-5 and F-11 carry over unchanged).

---

## 6. Proposed roadmap

| Phase | Deliverable | Exit criterion |
|---|---|---|
| **0. Spike** (1-2 wk) | Rust `orchard`/`pczt` no_std on `xtensa-esp32s3-espidf`, linked into IDF; PSRAM on; 240 MHz | Timings for RedPallas, cmx, commit_ivk and heap; go/no-go for Rust vs C |
| **1. Keys** | ZIP 32 Orchard + BIP44/133 from the existing vault; seed fingerprint; UFVK export over USB; UA display (ZIP 316, F4Jumble) | UFVK and UA match `zcash_keys` for the ZIP 32 test vectors; Zodl/Zallet import gives the same addresses |
| **2. Sign (USB, testnet)** | Chunked PCZT upload; v5 + v6 sighash; check/display/sign for Ironwood + transparent; branch ID whitelist (NU6.3, NU7) | Real testnet sends through the Rust CLI host; sim conformance vectors against librustzcash; fuzz the PCZT parser |
| **3. Orchard turnstile + migration** | Orchard spends with ZIP 326 dummy outputs; batch signing (PCZB/PCZS) | ZIP 318 migration on testnet |
| **4. Airgap** | CAM board + larger panel; `zcash-accounts` / `zcash-pczt` UR | Pair and send with Zodl on testnet |
| **5. Harden** | Burn flash encryption + secure boot on a dev unit; JTAG off; optional ATECC608B PIN gate; external review | LeekWallet T11 finally executed on silicon |
| **6. Companion + swaps** | Tauri app: sync via Zaino/Zebra, NEAR 1Click confidential swaps | Swap ZEC → asset on mainnet with a small amount |

**Deliberately out of scope for v1:**
- Sapling spends
- Sprout
- ZSAs (not scheduled)
- TEX multi-step proposals (Zodl refuses them for hardware wallets too)
- ZIP 48 multisig
- FROST

---

## 7. References

**Local code:**
- `pczt/src/roles/signer/mod.rs` (signer duties)
- `pczt/src/orchard.rs` (field docs, dummy_sk rule)
- `pczt/tests/firmware_compat.rs` (Keystone vector)
- `zcash_client_backend/src/data_api/wallet.rs` L3375-3572 (signer redaction views)
- `zcash_keys/src/keys.rs` (USK/UFVK)
- `zcash_protocol/src/consensus.rs` (branch IDs, heights)
- `zodl-android/ui-lib/.../KeystoneProposalRepository.kt` (complete hardware-wallet flow)
- `.../NearApiProvider.kt` (1Click)

**Prior art:**
- https://github.com/KeystoneHQ/keystone3-firmware (`rust/apps/zcash`, `rust/zcash_vendor`)
- https://github.com/LedgerHQ/app-zcash
- https://github.com/wh00hw/libzcash-ironwood-c
- https://github.com/wh00hw/zcash-hw-wallet-esp32
- https://github.com/jarys/ztrezor/blob/main/doc/implementation.md

**Specs:**
- ZIP 32, 229, 244, 258, 316, 318, 326, 374, 2005: https://zips.z.cash/
- NEAR confidential swaps: https://docs.near-intents.org/integration/distribution-channels/1click-api/quickstart/confidential-swaps

**Toolchain:**
- https://docs.espressif.com/projects/rust/book/
- https://github.com/esp-rs/rust/issues/277 (Xtensa ICE)

**Treat as uncertain:**
- NU7 mainnet date
- 1Click deposit address type
- all ESP32 performance figures (no published benchmarks)
- the NU6.2 date (from secondary sources)
