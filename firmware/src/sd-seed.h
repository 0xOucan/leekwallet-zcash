/**
 * SD card storage mode (roadmap 1.4): load a seed from a .zkv file, or save
 * the open seed to one.
 *
 * A loaded seed becomes the session's temporary seed
 * (wallet_use_temporary_mnemonic): nothing reaches flash, a lock or a reboot
 * forgets it, and it derives exactly what the same phrase typed in or stored
 * in the vault derives. The card and its passphrase are the storage.
 *
 * The phrase exists in clear only inside these calls, and is wiped before
 * they return.
 */
#ifndef LEEK_SD_SEED_H
#define LEEK_SD_SEED_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sdcard.h"
#include "zkv.h"

typedef enum {
    SDSEED_OK = 0,
    SDSEED_E_NOCARD,    /* no card, or it would not mount */
    SDSEED_E_IO,        /* read or write failed */
    SDSEED_E_FULL,      /* SEED1..SEED99 all taken */
    SDSEED_E_FORMAT,    /* not a seed file this firmware reads */
    SDSEED_E_AUTH,      /* wrong passphrase, or the file was altered */
    SDSEED_E_PASS,      /* passphrase not 8-64 printable ASCII */
    SDSEED_E_WALLET,    /* no seed open to save, or the vault refused the load */
    SDSEED_E_VERIFY,    /* the file written did not read back */
} SdSeedResult;

/** Mount if needed and list the .zkv files. Count, or -1 with no card. */
int sdseed_list(char names[][SDCARD_NAME_MAX + 1], int max);

/**
 * Open `name` with `pass` and make its phrase the session's seed. On success
 * `meta` holds the file's birthday, account, flags and label; its entropy is
 * wiped. The vault must be unlocked.
 */
SdSeedResult sdseed_load(const char *name, const char *pass, size_t pass_len, ZkvContents *meta);

/**
 * Seal the open seed's phrase (stored or temporary) under `pass` into a new
 * SEEDn.ZKV, then read it back and open it before reporting success.
 * `iterations` 0 means ZKV_ITERATIONS_DEFAULT.
 */
SdSeedResult sdseed_save(const char *pass, size_t pass_len, const char *label, uint32_t birthday,
                         uint32_t account, uint32_t iterations, char out_name[SDCARD_NAME_MAX + 1]);

const char *sdseed_strerror(SdSeedResult r);

#endif /* LEEK_SD_SEED_H */
