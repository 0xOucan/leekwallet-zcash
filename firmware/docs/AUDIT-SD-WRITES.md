# Storage writes audit — can a failure be read as "empty"?

Prompted by a Zcash project's post-mortem (Konclave): small files written in place meant a crash or a
full disk could leave a file that no longer read correctly, and for their key file "unreadable" was
read as "no key", which opened private reads. Their fix was to write through a temporary and a
rename, and to refuse to treat an unreadable file as empty.

This pass asks the same two questions of every write that holds wallet state here, on the microSD
card and in NVS:

1. What does a power cut, a reset, a pulled card or a full card at each step leave behind?
2. When that is later read back short, empty or garbled, can it be taken for "nothing there" — so
   that a new wallet is written over an old one, a seed is shown or exported, a PIN check is
   skipped, or a signature is written for the wrong request?

The answer for the card was "nearly safe already". The answer for NVS was not, and that is where
the serious findings are.

---

## 0. Summary

| | | |
|---|---|---|
| **N1** | An NVS vault that would not read was treated as a new device: erased at boot, or offered PIN setup, with every PIN try spent towards a wipe | Fixed |
| **N2** | The v1/v2 → v3 KDF migration was not crash-safe: a cut refused the right PIN, and for v2 could destroy the only salt | Fixed |
| **S1** | A seed file that failed its read-back stayed on the card under its `SEEDn.ZKV` name | Fixed |
| **S2** | An earlier request's `signed.zsig` stayed beside a new `unsigned.pczt` | Fixed |
| **S3** | `.zsig` writes were not read back; the sim's rename overwrote where FATFS refuses | Fixed |
| **D1** | `init_vault_params_v3()` claimed one commit made three NVS writes atomic | Fixed (comment) |
| **L1** | `wallet_delete_mnemonic()` is not atomic (unreachable from the UI or protocol) | Documented |
| **L2** | `wallet_add_mnemonic()` cut between slot and count leaves orphan ciphertext | Documented |
| **L3** | The PIN attempt counter reads any error as "absent", i.e. full attempts | Documented |
| **I1** | What a read-back can and cannot prove on an SD card | Documented |

New suites: `test_vault_unreadable`, `test_vault_migrate`. Extended: `test_sd_seed`, `test_sd_sign`,
`test_ui`. Section 4 lists commands and results.

---

## 1. What was checked

Every call that changes storage, found by `grep` for `fopen`, `fwrite`, `f_write`, `rename`,
`unlink`, `nvs_set_*`, `nvs_erase_*` and `nvs_flash_erase` across `src/` and `components/`.

**SD card** (`src/sdcard.c`, the only file that touches the card):

| File | Writer | How |
|---|---|---|
| `SEEDn.ZKV` | `sd-seed.c` → `sdcard_write_new()` | `ZKVWRITE.TMP`, `fsync`, `rename` to a free name; never overwrites |
| `LEEKZ/signed.zsig` | `sd-sign.c` → `sdcard_write_replace()` | `signed.zsig.tmp`, `fsync`, `unlink` old, `rename` |
| `LEEKZ/unsigned.pczt` | the companion | read only |

`airgap.c` writes nothing to the card (the QR path ends in a UR on screen). There is no other card
write.

**NVS** — the `colibri` vault (`components/leek-wallet/leek-wallet.c`: record, salt, parameters,
version, mnemonic slots, counts), the PIN attempt counter (`pin.c`), the wipe marker
(`device-wipe.c`), per-account metadata (`zcash-meta.c`) and plain settings (`ui.c`, `ble-name.c`,
`transport.c`).

**FATFS rename.** ESP-IDF's `vfs_fat_rename` calls FatFs `f_rename`, which returns `FR_EXIST` (errno
`EEXIST`) when the target exists — checked in both IDF trees under `~/.platformio/packages`
(`ff.c`, `f_rename`). So a rename never replaces a file on the card, which is what
`sdcard_write_new()` relies on and why `sdcard_write_replace()` must unlink first. Within one
directory `f_rename` registers the new entry and removes the old in the same directory sector
before syncing, so the rename itself does not leave a half state worth modelling.

**NVS semantics.** Each `nvs_set_*` is written to flash when it is called; `nvs_commit()` does not
group writes (`nvs_api.cpp`). One entry is power-safe — a duplicate left by a cut is resolved at
the next mount (`nvs_pagemanager.cpp`). A *sequence* of entries is not.

**Host binding of `.zsig`.** Every reader checks the SHA-256 in the `.zsig` header against the PCZT
it applies the signatures to: `host/leek-device/src/zsig.rs` (`parse_for`, `check_hash`),
`host/zecz-core/src/wallet.rs` (`finish`), `host/leek-zcash/src/apply.rs` (`apply_zsig`) and
`host/leek-android/src/lib.rs` (`apply_signatures_to`). On the board the hash written is the one
taken at load, and the file is hashed again before signing (`SDSIGN_E_CHANGED`). A stale `.zsig`
therefore cannot sign the wrong transaction; S2 is about the card's contents, not a signature
bypass.

---

## 2. Findings

### N1 — an unreadable vault read as an empty one

Three doors, one bug class.

- `wallet_init()` and `app_main()` called `nvs_flash_erase()` on `ESP_ERR_NVS_NO_FREE_PAGES` and
  `ESP_ERR_NVS_NEW_VERSION_FOUND` — the ESP-IDF example boilerplate. Here that partition is the
  vault: a downgrade across an NVS format, or a partition that would not mount, became a
  factory-new device.
- `load_vault_record()` read a record of the wrong length, version or generation as *absent* and
  fell back to the legacy layout. `erase_stale_slots()` then ran at boot and, believing the vault
  legacy, erased every `m0_N`/`m1_N` slot — the seeds, before anyone typed a PIN. A downgrade to a
  firmware with an older `VAULT_REC_VERSION`, or one damaged record entry, was enough.
- Seeds with no verifier reported `password_set = false`. `pin_is_set()` follows that, so the UI
  offered PIN setup over them; and if a verifier was present but uncheckable, every PIN try was
  charged as wrong, so the owner's correct PIN walked the device to a wipe.

**Fix.** The vault has an unreadable state (`wallet_storage_unreadable()`): NVS that will not
mount, a record present but not readable (anything but `NOT_FOUND`), generation slots with no
record, or mnemonics with no verifier. The last two are decided by `vault_contradicts_layout()`,
which relies on the vault's own ordering: a password is always published before the first
mnemonic, and a legacy PIN change keeps `m_N` until its record lands, so no interrupted operation of
this firmware produces either state. While unreadable, the vault reports a password, skips both
boot sweeps, and refuses `wallet_set_password`, `wallet_unlock` and `wallet_change_password`
(`WALLET_ERROR_STORAGE_FAILED`); `pin_verify()` returns before spending an attempt; and the UI shows
`SCREEN_STORAGE_FAULT` ("Storage unreadable / Nothing was erased") from boot and from any path to
the PIN prompt. `app_main()` no longer erases or aborts. Undoing the damage — a newer firmware, a
repaired record — opens the same vault with the same PIN; `test_vault_unreadable` checks exactly
that after each break.

### N2 — the KDF migration was not crash-safe

The first unlock on a v1 or v2 vault re-encrypts it to v3. The code described the ordering as
"rewrite, flip the version marker last", but:

- `init_vault_params_v3()` wrote salt, parameters and `kdf_ver = 3` before any blob was rewritten,
  trusting one `nvs_commit()` to make them atomic. It does not.
- For v2 it minted a fresh salt over the one the blobs were encrypted under.
- `load_vault_params()` read a staged parameter blob under a v1 marker as v1's own.

Measured with the fake NVS's crash injection: on a v1 vault, cuts after writes 2–3 of 5 made the
right PIN fail; on v2, writes 1–3. Each failure costs a PIN attempt, and for v2 the old salt is
gone, so seeds not yet rewritten are unrecoverable.

**Fix** (`migrate_vault_to_current()`): v1 stages its salt and parameters without the marker (both
ignored under a v1 marker), resumes onto a staged salt rather than minting another, rewrites each
slot in place, publishes the new verifier, and writes the marker last; `unlock_matches()` accepts
the staged derivation's verifier for the one window between those two and finishes the flip — in
`wallet_verify_password()` too, so `pin.c` never charges an attempt that unlock would accept. v2
keeps its salt and key and only changes the blob format. `load_mnemonic_with_alt()` reads a slot in
either form under either key. `test_vault_migrate` builds v1 and v2 vaults as those firmwares wrote
them (CBC blobs, unsalted and salted) and cuts the first unlock after every write, then the retry
after every write: 146 runs, all open with the PIN; the old code fails 332 checks.

### S1 — a failed seed save left a file that looked like a backup

`sdseed_save()` read the new file back and opened it, but on failure left it on the card under its
real name. A damaged `.zkv` fails the same GCM tag a wrong passphrase does
(`zkv_strerror(ZKV_E_AUTH)`: "wrong passphrase or damaged file"), so later its owner would be told
they had forgotten a passphrase. **Fix:** `sdcard_write_new()` reads the file back through a fresh
handle and removes the name on a mismatch (`SDCARD_E_VERIFY`); `sdseed_save()` removes a file that
reads back but will not open. The name was free a moment before, so only this call's file is ever
removed.

Unchanged and confirmed: a cut before the rename leaves `ZKVWRITE.TMP`, removed at the next mount;
names are never reused over an existing file; a truncated, empty or garbled `.zkv` is refused
(`SDSEED_E_FORMAT`/`_AUTH`) and loads nothing — it is never skipped or taken as "no card".

### S2 — a stale `signed.zsig`

The previous request's `signed.zsig` stayed on the card beside a new `unsigned.pczt` until the next
successful signing. After a rejected or failed signing it looked like the answer. **Fix:**
`sdsign_load()` removes a `signed.zsig` that does not name the loaded file, and any
`signed.zsig.tmp`, before the review — and refuses the load if the card will not let them go. A
`.zsig` for this very file is kept, so rejecting a second review costs nothing.

### S3 — `.zsig` not read back; the sim's rename disagreed with FATFS

`sdcard_write_replace()` now reads back too. The host build's rename refuses an existing target, as
`f_rename` does, so a suite cannot pass on a rename the board would refuse. A cut between the
`unlink` and the `rename` leaves a complete `.tmp` and no `signed.zsig`: never a short file under the
real name. The `.tmp` is never promoted (nothing on the card says it is whole); S2 deletes it.

### D1, L1–L3, I1

- **D1.** `init_vault_params_v3()`'s comment claimed atomicity; it now says what holds instead: only
  a vault with nothing encrypted calls it, and the marker is written last.
- **L1.** `wallet_delete_mnemonic()` shifts slots down, erases the last and then writes the count. A
  cut mid-way duplicates a slot or leaves the last index empty; no kept seed is lost. Nothing in the
  firmware calls it today.
- **L2.** `wallet_add_mnemonic()` writes the slot, then the count. A cut between leaves ciphertext
  under an index past the count until the next add overwrites it.
- **L3.** `pin_init()` takes any read error of the attempt counter, not only `NOT_FOUND`, as "never
  stored" and grants full attempts. A single NVS entry is power-safe, so reaching this needs
  physical flash access, which already defeats more than the counter.
- **I1.** A read-back proves FATFS and the card's controller return what was written; it cannot see
  past the card's own cache. `fsync` then `fclose` is as far as the host side can push it.

Also checked and left alone: `zcash-meta` reads an unreadable record as none (deliberately: the
birthday only saves a question), and the settings bytes default when absent.

---

## 3. What changed

- `src/sdcard.c`, `src/sdcard.h` — every card-changing call through one wrapper; read-back after
  rename (`SDCARD_E_VERIFY`); host rename refuses an existing target; a fault model for the sim
  (`sdcard__fault_at_step_for_test`: FAIL, CUT, TEAR, FLIP; `sdcard__card_space_for_test`).
- `src/sd-seed.c` — remove a written file that will not open.
- `src/sd-sign.c` — drop a stale `.zsig` and stray `.tmp` before the review.
- `components/leek-wallet/` — the unreadable state (N1); the migration (N2); `wallet_storage_unreadable()`.
- `src/pin.c` — no attempt spent on an unreadable vault.
- `src/ui.c`, `src/ui.h` — `SCREEN_STORAGE_FAULT`, from boot and from the PIN prompt's entry.
- `src/main.c`, `src/device-wipe.c` — no erase on a mount failure; comments that relied on it.
- `docs/VAULT.md` — the migration's write-by-write ordering.
- `sim/` — `test_vault_unreadable.c`, `test_vault_migrate.c`, new cases in `test_sd_seed.c`,
  `test_sd_sign.c`, `test_ui.c`; `fake_nvs_fail_init()` and `fake_nvs_erase_count()`.

The on-card formats (`.zkv`, `.zsig`) and the NVS layout are unchanged; nothing needed a version.

---

## 4. Verification

```
make -C sim test                     # every suite, test_zsign_diff included
pio run -e esp32s3cam                # never uploaded
pio run -e esp32s3cam-color
```

On this tree `make -C sim test` exits 0: every suite in `TESTS` passes, the slow
`test_zsign_diff` included, and the two new ones report `PASSED (0 failures)`. Both firmware
environments build. The new suites were also run against the vault as it was before this pass:
`test_vault_unreadable` fails 78 checks (the first case alone: seeds erased at boot, PIN setup
offered, an attempt spent on the right PIN) and `test_vault_migrate` fails 332.

---

## 5. What remains

- **Recovery from N1 needs a decision.** The fault screen has no way out on purpose. A downgrade is
  fixed by flashing the newer firmware; a truly damaged vault needs an explicit erase
  (`esptool erase_region` over the `nvs` partition, then restore from the backup). Whether the
  device should offer a deliberate, confirmed "erase and start again" from that screen is the
  owner's call — it must never be the default.
- **First flash over foreign data.** A board whose `nvs` region holds another firmware's NVS that
  will not mount now shows the fault instead of silently formatting. `flash-board.sh` never erases;
  such a board needs one explicit `erase_region`.
- **L1, L2** are left as they are; L1 should get the generation treatment if delete is ever wired up.
- **Hardware.** The sim proves ordering, not the card: a power cut on the S3 CAM mid-save and
  mid-sign, and a downgrade with a populated vault, are worth one run each on the bench.
