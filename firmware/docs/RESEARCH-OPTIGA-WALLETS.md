# OPTIGA Trust M in hardware wallets — research brief

**Status: desk research, 2026-11-10. Nothing here has been run on a chip.**
What two shipping wallets do with the same part this board carries, what the
Trust M offers and what it makes permanent, and what that means for an
ESP32-S3 without flash encryption. The design that builds on it is in
[RESEARCH-SECURE-ELEMENT.md](RESEARCH-SECURE-ELEMENT.md#vault-options-nvs-sd-card-and-se-container).

Code was read from `trezor/trezor-firmware` at commit `95a7289c` (main,
2026-11-10) and `BitBoxSwiss/bitbox02-firmware` at commit `b27f5f68`
(master, 2026-11-04). Trezor's firmware is GPL-3.0: it is cited here for
design and constants only, and none of it is copied into this repository.
Infineon facts come from the Solution Reference Manual (SRM) v3.70 in
`Infineon/optiga-trust-m-overview`. Anything marked **[unverified]** was not
confirmed from a primary source.

---

## 1. Trezor Safe 3 / Safe 5 (OPTIGA Trust M V3)

### 1.1 Data objects used (current storage version ≥ 5)

Sources: `docs/core/misc/optiga.md` (rendered at docs.trezor.io), `core/embed/sec/optiga/optiga.c`, and `core/embed/projects/prodtest/cmd/prodtest_optiga.c`.

| OID | Name | Read | Change | Execute | Type | Purpose |
|---|---|---|---|---|---|---|
| E0E0 | CERT_INF | ALW | NEV | ALW | DEVCERT | Infineon factory certificate (left untouched) |
| E0E1 | CERT_DEV | ALW | NEV | ALW | DEVCERT | Trezor device certificate chain (written at factory) |
| E0F0 / E0F2 | KEY_DEV / KEY_FIDO | NEV | NEV | **Conf(E140)** | Sign | P-256 attestation keys. They work only over the shielded connection |
| E0F3 | PIN_ECDH | NEV | ALW | Luc(E121) | KeyAgree | P-256 key for an ECDH stretching step |
| E200 | PIN_CMAC | NEV | ALW | Luc(E121) | Enc | AES-256 CMAC stretching key, generated on-chip (`GenSymKey`) |
| E120 | STRETCHED_PIN_CTR | ALW | Auto(F1D0) | ALW | UPCTR | Limit of 16 guesses at STRETCHED_PIN |
| E121 | PIN_TOTAL_CTR | ALW | NEV | ALW | UPCTR | Lifetime limit of 600 000 stretch operations |
| E122 | PIN_HMAC_CTR | ALW | Auto(F1D4) | ALW | UPCTR | Limit of 16 uses of PIN_HMAC |
| E140 | KEY_PAIRING | NEV | NEV | ALW | PTFBIND | 32-byte platform binding secret |
| F1D0 | PIN_SECRET | Auto(F1D4) | ALW | ALW | AUTOREF | Random 32-byte secret, released only after a correct PIN |
| F1D4 | STRETCHED_PIN | NEV | Auto(F1D0) | Luc(E120) | AUTOREF | HMAC-derived digest of the stretched PIN |
| F1D8 | PIN_HMAC | NEV | Auto(F1D4) | Luc(E122) | PRESSEC | Counter-protected HMAC-SHA256 stretching key |
| F1D1 | PIN_HMAC_V4 | NEV | NEV | Luc(E121) | PRESSEC | Deprecated key from storage v3/v4 |

In PRODUCTION builds, `optiga_set_metadata()` also sets `LcsO = operational` on each object. Once that is done, the access conditions can no longer be changed (see §3.3).

### 1.2 PIN set and verify flow

From `optiga.c` and `storage/storage.c`:

1. **MCU stretch.** `mcu_pin_stretch()` runs PBKDF2-HMAC-SHA256 with 20 000 iterations over the PIN. The salt is `hardware_salt ‖ storage_salt(32 B) ‖ optional ext_salt`. On the STM32U5 (Safe 5) the result is also passed through the hardware-key AES (`USE_STORAGE_HWKEY`, `secure_aes_ecb_encrypt_hw`).
2. **CMAC/ECDH stretch** (`optiga_pin_stretch_cmac_ecdh`). The loop runs `PIN_STRETCH_ITERATIONS` = 2 times:
   - `digest = HMAC(sp, "")`
   - `cmac = CMAC(E200, digest)` and `ecdh = ECDH(E0F3, hash_to_curve(digest))`
   - `sp = HMAC(sp, cmac‖ecdh)`

   Every operation increments E121.
3. **Set** (`optiga_pin_set`):
   - Generate a random `hmac_stretching_secret` and compute the HMAC step offline: `sp = HMAC(sp, prefix‖HMAC(key, HMAC(sp,"")))`.
   - Write a random `PIN_SECRET` to F1D0, which is always writable.
   - Authorise with F1D0 (Auto). Reset E120 to the limit, then write `HMAC(sp,"")` into F1D4.
   - Compute `sp = HMAC(sp, PIN_SECRET)`.
   - Authorise with F1D4. Write the HMAC key into F1D8 and reset E122 to 16.
4. **Verify** (`optiga_pin_verify`):
   - `optiga_pin_stretch_hmac()`: an `EncryptSym` HMAC on F1D8. This increments E122 and returns `ACCESS_COND` once the counter is exhausted.
   - `optiga_set_auto_state(F1D4, HMAC(sp,""))`: this increments E120. The outcome is `AUTH_FAIL` (wrong PIN), `CTR_LIMIT` (locked) or success.
   - On success, reset E122, read PIN_SECRET from F1D0, compute `sp = HMAC(sp, PIN_SECRET)`, authorise with F1D0, and reset E120.
   - The final `sp` is the KEK that decrypts the DEK on the MCU (`derive_kek_set` / `decrypt_dek`).
5. **Auto state mechanics** (`optiga_commands.c::optiga_set_auto_state`):
   - `GetRandom` places a 16-byte nonce into the session context (E100).
   - `DecryptSym` (0x95) in HMAC-SHA256 mode then verifies `HMAC(secret, nonce)` against the AUTOREF object.
   - The secret never crosses the bus. Only a fresh MAC does.
   - Auto states are cleared explicitly afterwards (`optiga_clear_all_auto_states`), as the SRM recommends.
6. **Counter reset encoding.** `optiga_reset_counter(oid, limit)` writes 8 bytes: a 4-byte count of 0 followed by a 4-byte threshold of `limit`.

**Why each piece exists** (stated rationale in `optiga.md` and code comments):

- Each stretch step sends only a hash of the stretched PIN to the chip. Someone sniffing the bus therefore cannot learn it.
- `PIN_SECRET` is mixed in on the MCU. Trezor says this means that even if OPTIGA and its link are fully compromised, a high-entropy PIN is no weaker than on a device with no OPTIGA.
- E122 blocks the "PIN-only" attack: overwrite F1D0, then reset E120 for unlimited guesses. Resetting E122 needs the old STRETCHED_PIN, and an attacker who overwrites that object loses the ability to check guesses at all.
- CMAC/ECDH and E121 force every guess through the chip and cap total guesses. They also mean an attacker must extract several differently-typed secrets before an offline search is possible.

### 1.3 Attempts, wipe, MCU/OPTIGA counter sync

- `PIN_MAX_TRIES` is 16, or 10 when TROPIC01 is present (Safe 7) (`storage/storage.h`).
- The MCU increments its own fail counter **before** checking the PIN (`unlock()`). When `ctr+1 >= PIN_MAX_TRIES` it calls `storage_wipe()`, which wipes norcow, i.e. the MCU flash storage.
- `storage_get_pin_rem()` keeps the two counters in step: it advances whichever of the MCU counter and the OPTIGA counter is behind.
- OPTIGA objects are not explicitly erased on wipe. The secrets become unusable once the counters hit their thresholds, and they are overwritten at the next `optiga_pin_set`.
- Trezor's user docs say that after 16 wrong attempts the SE "erases the secret" and the device resets ([trezor.io guide](https://trezor.io/guides/trezor-devices/trezor-fundamentals/pin-protection-on-trezor-devices)).
- Safe 7 (TROPIC01 together with OPTIGA) uses `STRETCHED_PIN_COUNT = 10`. That design chains 10 AUTOREF slots (F1D4, F1D1–F1D3, F1D5–F1D7, F1D9–F1DB) and stores an `OPTIGA_HMAC_RESET_KEY` in MCU storage.

### 1.4 Pairing secret (platform binding)

- **Factory step.** `prodtest optiga-pair`:
  - Loads the secret through `secret_key_optiga_pairing()`.
  - If the handshake fails, sets E140 to `PTFBIND` with change ALW and writes the secret.
  - Re-runs the handshake to confirm.

  `optiga-lock` then:
  - Deletes the temporary trust anchor in E0E8.
  - Sets E140 to read NEV / change NEV, with `LcsO = op`.
  - Locks the certificates.
  - Sets KEY_DEV/KEY_FIDO execute to `Conf(E140)`.

  The FIDO key is imported at the factory in encrypted form (`optiga-keyfido-write`), using ECDH with KEY_DEV and a temporary trust anchor.
- **MCU-side storage.**
  - Safe 3 (STM32F4): the 32-byte secret sits in a dedicated flash "secret" sector, guarded by the MPU (`sec/secret/stm32f4/secret.c`). Installing unofficial firmware erases it (`secret_unlock_bootloader()`).
  - Safe 5 (STM32U5): the secret is derived with `secret_key_derive_sym(SECRET_PRIVILEGED_MASTER_KEY_SLOT, KEY_INDEX_OPTIGA_PAIRING)` (`sec/secret_keys/stm32u5/secret_keys.c`).
- **Runtime behaviour.** `optiga_init_and_configure()` attempts the shielded handshake. If it fails, the code calls `optiga_soft_reset()` and **continues without the shielded connection**. Only attestation and FIDO stop working, because they depend on `Conf(E140)`. **None of the PIN objects is gated on Conf(E140)**, so PIN security does not depend on the pairing secret staying secret.

### 1.5 Other details

- **SEC handling.**
  - `optiga_read_sec()` reads the security event counter (E0C5).
  - Time estimates in `optiga_*_time()` model throttling, so the UI can show progress.
  - `optiga_suspend()` delays power-down while SEC exceeds a threshold, so the SEC can decay (`optiga_init.c`).
  - `optiga_set_sec_max()` deliberately sends an invalid ECDH point to drive SEC to its maximum. Its callers were not traced **[unverified]**.
- **ECDSA masking.** `optiga_sign()` masks the digest with an MCU-held scalar for the FIDO key (`SECRET_KEY_MASKING`). This plausibly mitigates EUCLEAK-style nonce/inversion leakage **[intent unverified]**.

### 1.6 Claimed model and critiques

- **Trezor's claims.** The SE holds a secret that is released only on the correct PIN and is erased after 16 failures. The chip is CC EAL6+ and NDA-free. The seed stays encrypted on the MCU ([Trezor SE article](https://trezor.io/learn/security-privacy/how-trezor-keeps-you-safe/secure-elements-in-trezor-safe-devices)).
- **Critiques.**
  - The seed lives on the MCU, not in the SE.
  - The SE firmware is closed.
  - Ledger Donjon (reported 2024-11-12, published 2025-03) voltage-glitched the Safe 3's STM32F4, gaining flash read/write and bypassing firmware-integrity and supply-chain checks. No PIN or seed was extracted ([Trezor advisory](https://trezor.io/vulnerability/donjon-s-trezor-safe-3-evaluation); press: [The Block](https://www.theblock.co/amp/post/346018/trezor-discloses-vulnerability-safe-3-crypto-wallet-rival-ledger)).
  - Ledger's view that "the MCU must be replaced" for Safe 3 and 5 is reported in press coverage only **[unverified primary]**.
  - The result is consistent with the design: an MCU dump yields ciphertext, salt and probably the pairing secret, but every guess still needs the chip.

## 2. Other wallets and projects

- **BitBox02 Nova** uses an OPTIGA Trust M V3 ([datasheet](https://bitbox.swiss/bitbox02/nova/BitBox02_Nova_BTC_Datasheet.pdf)); the original BitBox02 uses the ATECC608A/B. Its open code is in `src/optiga/optiga.c`, `optiga.h` and `src/rust/bitbox-securechip/src/optiga.rs`.
  - **Objects.** E140 holds the PBS. E200 is a CMAC key with Luc(E120), where E120 is limited to 590 000. F1D0 is an HMAC key. F1D4 is a write-protected HMAC key with Luc(E122). F1D2 is `PASSWORD_SECRET`. F1D3 is `PASSWORD` (AUTOREF) with Luc(E121). The attempt limit is 10 (`MAX_UNLOCK_ATTEMPTS`).
  - **Shielded connection.** Unlike Trezor, almost every object adds `Conf(E140)`, i.e. use is allowed only over the shielded connection.
  - **Where the PBS comes from.** It is the MCU "io_protection_key" from MCU flash (`src/optiga/pal/pal_os_datastore.c`).
  - **Stretch (V1).** Each line feeds the next:
    1. `CMAC(E200, salted(pw))`
    2. → `HMAC(F1D4)`, which consumes the small counter
    3. → Auto(F1D3)
    4. → read F1D2
    5. → `HMAC(F1D2, auth_pw)`
    6. → `HMAC(salted_out(pw), …)`
  - **Locking.** `FINAL_LCSO_STATE_*` is set to operational, with a comment that metadata cannot be changed after that. Factory setup lives behind `FACTORYSETUP`.
  - **Takeaway.** This is a second independent design built on the same primitives: AUTOREF, Luc counters and a password secret mixed in on the MCU.
- **Trezor Safe 7.** It pairs OPTIGA Trust M with TROPIC01, which is open-architecture ([Trezor](https://trezor.io/guides/trezor-devices/trezor-safe-7/how-trezor-safe-7-s-three-hardware-layers-protect-your-wallet)).
- **Infineon Blockchain Security 2Go.** This is a separate NFC smart-card product, not Trust M. It holds up to 255 secp256k1 keys with PIN authentication ([Infineon/Blockchain](https://github.com/Infineon/Blockchain), [blocksec2go](https://pypi.org/project/blocksec2go/)). It is not usable for Pallas.
- **Blockstream Jade** (ESP32/ESP32-S3, no SE). It uses a "virtual secure element": the PIN is combined with a secret held on the device and one held by a remote blind-oracle PIN server, which can be self-hosted. The design relies on ESP32 flash encryption and secure boot ([Jade security FAQ](https://help.blockstream.com/blockstream-jade/faqs/jade-security-model-faqs), [blog](https://blog.blockstream.com/jade-virtual-secure-element/)). This is the closest ESP32 analogue, and it does burn eFuses.
- **Comparisons [unverified, from general knowledge]:** Passport and Coldcard use Microchip ATECC608 parts (Coldcard Mk4 adds a DS28C36B). Keystone uses multiple SEs. Ledger uses an ST33 running BOLOS. I found no other open-source wallet using OPTIGA Trust M for PIN gating. `bootstrap-prime/optiga-trust-m` is hardware notes only.

## 3. OPTIGA Trust M capabilities that matter here

All points in this section come from the SRM v3.70 and the object dumps in `Infineon/optiga-trust-m-overview` unless another source is named.

### 3.1 Objects and sizes

- **Arbitrary data objects.**
  - F1D0–F1DB: type 3, 140 bytes each.
  - F1E0–F1E1: type 2, 1500 bytes each.
  - Default `LcsO = cr`. Access conditions are application-specific.
  - In the V3 dump, the default is change `LcsO<op` and read ALW.
- **Monotonic counters.** E120–E123, 8 bytes each (UPCTR: count plus threshold), default change `LcsO<op`. The SRM advises that a *linked* counter should have CHA = NEV to avoid DoS. Trezor and BitBox deliberately use `Auto(...)` instead, so a correct PIN can reset it.
- **Platform binding secret E140.** 64 bytes maximum; 32 bytes or more recommended.
  - Default V3 ACs: change `LcsO<op || Conf(E140)`, read `LcsO<op`. In other words, it is **readable until locked**.
  - The SRM says to set `LcsO = op` with CHA = NEV and RD = NEV.
  - Infineon's host library ships a placeholder PBS of `0x01…0x40` (`extras/pal/*/pal_os_datastore.c`).
- **Keys.**
  - E0F0 is the Infineon device key: change NEV, used with E0E0.
  - E0F1–E0F3 are ECC keys; E0FC/E0FD are RSA keys.
  - E200 is AES (V3 only).
  - E100–E103 are session contexts.
- **System objects.** E0C0 global LCS; E0C2 UID; E0C5 SEC (read ALW); E0C9 security monitor configuration (V3; change `LcsO<7`).

### 3.2 Access conditions

- **Basic conditions.** ALW, NEV, LcsG/LcsA/LcsO(op,value), Conf(X), Int(X), Auto(X), SecStaG/A. There is also the access-driven **Luc(counter)**: each EXE advances the counter and is refused at the threshold.
- **Auto(X).** Satisfied by an HMAC verification against an AUTOREF object. Up to 4 Auto states can be active at once. The SRM says to clear them manually.
- **Combining conditions.** Complex ACs combine simple ones with `&&` and `||`.
- **V1 limits.** OPTIGA Trust M **V1 lacks Auto, AUTOREF, HMAC, AES/E200, HKDF, MUPD and SecSta**. The Trezor/BitBox pattern therefore **needs V3** (or Express/MTR, which are built on V3).
- **Data types.** BSTR, UPCTR, TA, DEVCERT, PRESSEC (HMAC/derive key), PTFBIND, UPDATSEC, AUTOREF.
- **Secret size and use.** HMAC or derivation secrets should be at least 16 bytes. The SRM recommends limiting their use to about 2048 operations via a counter.

### 3.3 Lifecycle and irreversibility

- **States.** Each lifecycle field moves only forward: cr(0x01) → in(0x03) → op(0x07) → te(0x0F).
- **Locking metadata.** The metadata tags D0/D1/D3 (ACs), E1 (key usage) and E8 (data type) are changeable only while `LcsO < op`. **Setting `LcsO = op` freezes the access conditions permanently.** The exception is a protected metadata update (MUPD, tag D8), which needs a signed manifest plus a preset reset type (F0); that is V3 only.
- **Termination.** A protected update can move an object to `te`, after which it is unusable.
- **What locking does not freeze.** Locking fixes the ACs, not the data. An object locked with change ALW stays writable.

### 3.4 Crypto and transport

- **Crypto.**
  - ECC P-256/384, plus P-521 and Brainpool on V3; RSA 1024/2048.
  - ECDH via `CalcSSec`; HMAC-SHA256/384/512 (V3).
  - AES-128/192/256 ECB/CBC/CMAC (V3).
  - TLS-PRF and HKDF; a TRNG.
  - **No Pallas, secp256k1, Ed25519 or BLAKE2.**
- **Shielded connection** ([IFX I2C protocol v2.03 PDF](https://github.com/Infineon/optiga-trust-m-overview/tree/main/docs/pdf)).
  - It is a pre-shared-secret handshake that derives session keys. Trezor's transport implements the record layer as AES-CCM (`optiga_transport.c`).
  - The SRM says its security "is as high as typical microcontroller/Host side hardware security level". Put plainly, it is only as secret as the host's copy of the PBS.
  - It has no host-supplied nonce for freshness, so the SRM gives a separate procedure for the host to authenticate the chip.
- **Protected update.** `SetObjectProtected` takes a COSE-like manifest signed against a trust anchor (E0E8/E0EF), optionally encrypted with UPDATSEC. It can update data, keys or metadata (V3).
- **Hibernate.** `CloseApplication(hibernate)` saves the application context. It is **refused while SEC > 0** and costs 5 NVM cycles.

### 3.5 Security monitor

- **What it counts.** "Secret key use", "private key use", key derivation on persistent objects, and decryption failures in protected update all raise security events. A "suspect system behaviour" event sets SEC to 255.
- **Throttling.** One event per t_max (default 5 s) is permitted. SEC decrements once per idle t_max. Delays start at SEC ≥ 128 and reach t_max at 255.
- **Credit.** SEC_CREDIT (default maximum 5, held in RAM and cleared at reset) absorbs bursts.
- **Configuration.** On V3, t_max, the credit maximum and the sync count can be set in E0C9. Setting t_max to 0 disables the monitor. E0C9 is changeable while `LcsO < 7`.

### 3.6 Endurance

- Each monotonic counter allows **600 000 updates maximum**. The whole chip allows 2 million tearing-safe programming cycles.
- Every EXE on a Luc-linked object costs one extra cycle. One SEC increment-and-decrement pair costs two.
- Data retention drops to about 2 years after 20 000 cycles of the same data, and to 0.5 years after 40 000.
- A Trezor-style unlock rewrites E120 and E122 (counter resets) on every success. At several unlocks a day that is well within budget. Avoid per-keystroke or per-signature counter use.

### 3.7 Variants

- **V1 vs V3.** Both are SLS32AIA. V3 is required for this design.
- **Express and MTR** come pre-provisioned on V3. E140 and **F1D0 are already `LcsO = op`**, with change `Conf(E140) && Auto(F1D0)` and F1D0 typed AUTOREF. The per-chip PBS and AuthRef must be claimed from CIRRENT Cloud ID (Express) or Kudelski keySTREAM (MTR). F1D0 is occupied and E140 cannot be rotated without them.
- **Recommendation.** Plain V3 (e.g. SLS 32AIA010MK) is the right part for a DIY build.

## 4. Design patterns and pitfalls

1. **PIN check.** Copy Trezor's three-object pattern:
   - an AUTOREF `STRETCHED_PIN` with exec Luc(ctrA);
   - an AUTOREF `PIN_SECRET` that is read-gated by Auto(STRETCHED_PIN);
   - a PRESSEC `PIN_HMAC` with exec Luc(ctrB), whose change and ctrB reset are gated by Auto(STRETCHED_PIN).

   Mix `PIN_SECRET` and the HMAC output into the KEK on the MCU, so a total chip compromise reduces security only to MCU PBKDF2. Always send `H(stretched)` over I2C, never the stretched value itself.
2. **Counters.** A counter's change AC must never be ALW after locking, or anyone can reset it. Use Auto(·) from a secret that only a correct PIN can produce. Add a lifetime counter with change NEV, like Trezor's E121, to bound total guesses even across resets.
3. **Lock or it's theatre.** If you do not set `LcsO = op`, anyone with I2C access can rewrite metadata, for example making PIN_HMAC readable with read ALW, and the whole scheme collapses. Locking is irreversible, so:
   - use a sacrificial chip or board while iterating;
   - gate locking behind an explicit provisioning step, as Trezor does with `PRODUCTION` and BitBox with `FINAL_LCSO_STATE`;
   - verify the metadata before and after writing it, as `optiga_set_metadata()` does.
4. **Readable binding secret.** When the PBS sits in plaintext host flash, `Conf(E140)` protects only against a bus sniffer who has not dumped flash. Anyone with the flash image can open a shielded session. Do not let PIN security depend on it; Trezor's PIN objects deliberately omit Conf(E140). It is still worth having:
   - it raises the bar for pure bus attacks;
   - it ties attestation keys to the board.

   BitBox's choice to put Conf(E140) on everything assumes the MCU keeps the PBS secret.
5. **Genuineness check.**
   - Read E0E0, the Infineon certificate (sample V3 issuer: "Infineon OPTIGA(TM) Trust M CA 300").
   - Validate the chain to Infineon's published root and intermediate CAs ([Infineon certificate page](https://new-origin.infineon.com/cms/en/product/promopages/optiga_tpm_certificates); exact root names **[unverified]**).
   - Challenge-sign with E0F0, then verify with the certificate's public key.

   This detects counterfeit or emulated chips. It does not detect a MITM chip in an attacker-modified board if the attacker relays to a genuine chip. EUCLEAK (CVE-2024-45678; [paper](https://eprint.iacr.org/2024/1380)) states that Infineon's library ECDSA inversion is non-constant-time and claims Trust M is affected, though the authors had not tested it. Key cloning would need local EM side-channel equipment. Treat attestation as anti-counterfeit, not anti-lab.
6. **Usable without the chip.** Follow Trezor and degrade gracefully: if the handshake fails, continue. There are two hard choices:
   - **(a)** A wallet created in OPTIGA mode cannot be unlocked without that chip, so recovery is from the seed backup only.
   - **(b)** A separate "no-SE" mode uses only MCU PBKDF2.

   Bind the mode into the authenticated, encrypted blob header so it cannot be downgraded. Downgrading would not help an attacker anyway, because the KEK needs the chip's secret, but it must not cause a silent re-key. Never fall back automatically from OPTIGA mode to no-SE mode for an existing wallet.
7. **Wipe semantics.** The real limit is the chip's counter. The MCU-side fail counter and wipe can be bypassed by anyone who can write flash. Increment before checking, as Trezor does, and keep the two counters in sync.
8. **Pitfalls.**
   - The V1 part will not work.
   - Express/MTR parts have F1D0 and E140 pre-locked.
   - Auto states left set act as an open door until cleared or reset.
   - SEC throttling makes unlock take seconds: Trezor's estimate is about 131 ms per auto-state plus throttling.
   - Hibernate fails while SEC > 0.
   - Endurance limits apply.
   - Tearing: a power cut mid-sequence must leave a recoverable state. Order the writes carefully, as Trezor resets ctrB before reading F1D0.

## 5. Implications for an ESP32-S3 wallet without flash encryption

- **What a thief gets.** Anyone holding the board can dump the SPI flash: firmware, the encrypted seed blob, salts and any PBS stored there. Security then rests entirely on:
  - (i) the PIN's entropy times the MCU PBKDF2 cost, offline, if the chip's secrets are known; and
  - (ii) the OPTIGA counters, online, if they are not.

  The Trezor pattern is the right fit, because it is designed so that an OPTIGA compromise falls back to (i) and an MCU compromise falls back to (ii).
- **Seed on the ESP32 is fine.** The OPTIGA cannot do RedPallas anyway, and both Trezor and BitBox keep the seed on the MCU. The KEK must come from the OPTIGA flow, not from anything stored in flash.
- **The pairing secret is effectively public**, because flash is not encrypted. Do not gate PIN objects on Conf(E140). Optionally use Conf(E140) only for attestation-key use, as Trezor does.
  - If you ever accept burning *one* eFuse key block, the ESP32-S3 HMAC peripheral could derive the PBS and a hardware salt from a read-protected eFuse key. That does not require enabling flash encryption.
  - That is a policy decision (eFuse burns are irreversible), and fault-injection attacks on ESP32 eFuse protection have been published **[unverified for the S3]**.
- **No secure boot means firmware can be swapped.** An attacker can install firmware that logs the PIN the next time the owner uses the device (evil maid). The OPTIGA does not prevent this. Mitigations:
  - an attestation flow driven by the companion app, where the app verifies the E0E0/E0F0 challenge and a firmware hash reported by the device (weak without secure boot);
  - user education and tamper evidence.
- **The lifecycle lock is the only thing standing between an attacker and the PIN objects.** It must be applied on real units, irreversibly. Keep development or bench chips unlocked and clearly separated. A flag in firmware (like Trezor's `PRODUCTION`) should decide whether provisioning locks.
- **Budget.**
  - Unlock latency: roughly 1–3 s of OPTIGA operations, plus 20k-iteration PBKDF2 on the S3 **[timing unverified for ESP32-S3]**.
  - Endurance: about 3–5 object writes per unlock.
  - Ten to sixteen attempts with a lifetime cap such as Trezor's 600k is a reasonable target.
- **Recommended object map (plain V3).** This mirrors Trezor storage v5 and keeps OIDs compatible with Trezor's published table:
  - F1D0 = PIN_SECRET (AUTOREF), F1D4 = STRETCHED_PIN (AUTOREF), F1D8 = PIN_HMAC (PRESSEC);
  - E120/E122 = guess and HMAC counters (limit 10–16), E121 = lifetime counter (change NEV);
  - optional E200 CMAC and E0F3 ECDH stretch keys with Luc(E121);
  - E0E0/E0F0 left as Infineon factory items for the genuineness check.

---

## Sources

**Trezor**
- docs.trezor.io, Optiga data object configuration: https://docs.trezor.io/trezor-firmware/core/misc/optiga.html (repo: `docs/core/misc/optiga.md`)
- trezor-firmware @95a7289c:
  - `core/embed/sec/optiga/optiga.c`
  - `core/embed/sec/optiga/optiga_commands.c`
  - `core/embed/sec/optiga/optiga_init.c`
  - `core/embed/sec/optiga/optiga_transport.c`
  - `core/embed/projects/prodtest/cmd/prodtest_optiga.c`
  - `core/embed/sec/secret/stm32f4/secret.c`
  - `core/embed/sec/secret_keys/stm32f4/secret_keys.c`
  - `core/embed/sec/secret_keys/stm32u5/secret_keys.c`
  - `storage/storage.c`
  - `storage/storage.h`
  - `core/embed/sec/optiga/unix/certs/README.md`
- Trezor SE article: https://trezor.io/learn/security-privacy/how-trezor-keeps-you-safe/secure-elements-in-trezor-safe-devices
- Trezor PIN guide: https://trezor.io/guides/trezor-devices/trezor-fundamentals/pin-protection-on-trezor-devices
- Trezor Safe 7 layers: https://trezor.io/guides/trezor-devices/trezor-safe-7/how-trezor-safe-7-s-three-hardware-layers-protect-your-wallet
- Donjon evaluation advisory: https://trezor.io/vulnerability/donjon-s-trezor-safe-3-evaluation
- The Block (press): https://www.theblock.co/amp/post/346018/trezor-discloses-vulnerability-safe-3-crypto-wallet-rival-ledger

**BitBox**
- bitbox02-firmware @b27f5f68:
  - `src/optiga/optiga.c`
  - `src/optiga/optiga.h`
  - `src/optiga/pal/pal_os_datastore.c`
  - `src/rust/bitbox-securechip/src/optiga.rs`
- Nova datasheet: https://bitbox.swiss/bitbox02/nova/BitBox02_Nova_BTC_Datasheet.pdf

**Infineon**
- Solution Reference Manual v3.70: https://github.com/Infineon/optiga-trust-m-overview/blob/main/docs/OPTIGA%E2%84%A2%20Trust%20M%20Solution%20Reference%20Manual.md (sections: Security Monitor, Data and Key Store, Access Conditions, Life Cycle State, Metadata expression, Security Guidance)
- Overview README (V1/V3/Express/MTR): https://github.com/Infineon/optiga-trust-m-overview
- Object dumps: `data/object_dumps/trust_m3_json.txt` and `trust_m_express_json.txt` in the same repo
- Host library and PALs (including an `extras/pal/esp32_freertos` port): https://github.com/Infineon/optiga-trust-m (`extras/pal/libusb/pal_os_datastore.c`)
- Infineon CA certificates: https://new-origin.infineon.com/cms/en/product/promopages/optiga_tpm_certificates
- Blockchain Security 2Go: https://github.com/Infineon/Blockchain, https://pypi.org/project/blocksec2go/

**Other**
- EUCLEAK: https://eprint.iacr.org/2024/1380, https://ninjalab.io/eucleak/
- Blockstream Jade: https://help.blockstream.com/blockstream-jade/faqs/jade-security-model-faqs, https://blog.blockstream.com/jade-virtual-secure-element/
