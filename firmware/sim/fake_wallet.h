/**
 * Test control surface for the fake wallet. See fake_wallet.c.
 */

#ifndef FAKE_WALLET_H
#define FAKE_WALLET_H

#include <stdbool.h>
#include <stdint.h>

/** Empty device: no password, no seeds, locked. */
void fake_wallet_reset(void);

/**
 * Preload a seed as if it had been imported earlier, and select it.
 * Returns the 1-based wallet index, or 0 if there is no room.
 */
uint8_t fake_wallet_preload(const char *mnemonic);

/**
 * Make every derivation fail, to exercise the screens' error paths.
 *
 * Those paths are where an error can be mistaken for an address (AUDIT S8a),
 * and on real hardware they are almost unreachable on demand.
 */
void fake_wallet_fail_derivation(bool fail);

/**
 * Make only the master fingerprint fail, leaving addresses working.
 *
 * Separate from fail_derivation because the screens combine the two: an
 * address the device could derive next to a fingerprint it could not is the
 * case where a placeholder would be read as a value.
 */
void fake_wallet_fail_fingerprint(bool fail);

/**
 * Model the real vault's window between `unlocked` and `has_mnemonic`:
 * wallet_unlock() publishes `unlocked` before it decrypts the active seed,
 * and wallet_select_wallet() drops the seed before it loads the next one.
 * For the next `polls` calls of wallet_get_status() the seed is reported
 * absent and wallet_with_seed() answers WALLET_ERROR_NO_MNEMONIC, as the
 * real one does inside that window. 0xFFFFFFFF: never loads.
 */
void fake_wallet_seed_loading(uint32_t polls);

/**
 * What the real zcash_account_derive() meets in wallet_with_seed(): true
 * when the vault is unlocked and a seed is in RAM. fake_zcash.c asks it, so
 * the protocol suite sees the vault's refusals without paying for a seed.
 */
bool fake_wallet_seed_ready(void);

/** True once the UI has called wallet_mark_backup_verified for `index`. */
bool fake_wallet_backup_verified(uint8_t index);

#endif /* FAKE_WALLET_H */
