/**
 * The seed's copy in the OPTIGA's Level 1 container (development only).
 *
 * One of three places the seed can be kept (docs/BACKUP.md): the NVS vault,
 * which stays the master; .zkv files on an SD card; and this, one copy in
 * OPTIGA data object 0xF1E0 (components/leek-se-optiga, container format
 * LKSC). Level 1 leaves the chip unlocked, so the container is a backup
 * LOCATION, not extra security: anyone with the I2C bus can read or rewrite
 * it, and the copy is exactly as strong as the PIN and the vault's KDF.
 *
 * Built only for a board with the chip and an image with the container
 * compiled in (LEEK_HAS_SE && LEEK_SE_CONTAINER_WRITES, UI_HAS_SE_BACKUP in
 * ui.c). No shipped image has either.
 *
 * The copy is sealed the way the vault seals a seed, with the vault's own
 * code: the key is vault_derive_key_with() (PBKDF2-HMAC-SHA512, the vault's
 * default work factor, domain "leek-enc-v2") over the PIN and a salt of its
 * own, and the AEAD is vault_encrypt() (AES-256-GCM, fresh nonce). Its own
 * salt, stored beside it, is what lets it open on a board whose vault was
 * wiped or rebuilt: it needs the PIN it was made under, nothing from NVS.
 * Nothing about the seed is readable without that PIN: the fingerprint,
 * label and birthday are inside the ciphertext with it.
 *
 *   container payload ("LKSV" v1), at most SEBK_BLOB_MAX bytes
 *     0  4  magic "LKSV"
 *     4  1  version (1)
 *     5 20  KDF parameters, vault_params_serialize() (VAULT_PARAMS_BLOB_V1)
 *    25 16  salt
 *    41  .  vault_encrypt(plaintext): nonce(12) || ciphertext || tag(16)
 *
 *   plaintext v1
 *     0  1  version (1)
 *     1 32  ZIP 32 seed fingerprint of the phrase, no BIP39 passphrase
 *    33  .  the .zkv payload v1 (zkv_payload_encode(): entropy, birthday,
 *           account, flags, label), so SD and SE carry one format
 *
 * The header is not authenticated on its own; a changed salt or work factor
 * derives a different key and the GCM tag fails, which reads as "wrong PIN or
 * damaged", the same answer as for the vault.
 *
 * Every function blocks for a KDF (about half a second on the S3) and a few
 * chip commands; call it from the UI task with something on the screen.
 * Every buffer a phrase, entropy or key passed through is wiped before
 * return, on every path.
 */
#ifndef LEEK_SE_BACKUP_H
#define LEEK_SE_BACKUP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "vault-crypt.h"
#include "vault-kdf.h"
#include "zkv.h"

#define SEBK_MAGIC        "LKSV"
#define SEBK_VERSION      1
#define SEBK_HEADER_SIZE  (4 + 1 + VAULT_PARAMS_BLOB_SIZE + VAULT_SALT_SIZE)
#define SEBK_PLAIN_MAX    (1 + 32 + ZKV_PAYLOAD_MAX)
#define SEBK_BLOB_MAX     (SEBK_HEADER_SIZE + VAULT_CRYPT_OVERHEAD + SEBK_PLAIN_MAX)
/* Work factors a copy may declare. The floor is the vault's own; the ceiling
 * keeps a planted blob from holding the device for minutes. */
#define SEBK_ITER_MIN     VAULT_KDF_V2_ITERATIONS
#define SEBK_ITER_MAX     100000u

typedef enum {
    SEBK_OK = 0,
    SEBK_E_NOCHIP,      /* no secure element answered */
    SEBK_E_IO,          /* the link failed or the chip refused a command */
    SEBK_E_EMPTY,       /* nothing in the container */
    SEBK_E_FOREIGN,     /* the object holds data that is not a LeekWallet copy */
    SEBK_E_DAMAGED,     /* a LeekWallet container, cut or corrupted */
    SEBK_E_PIN,         /* this PIN does not open the copy (or it was altered) */
    SEBK_E_OTHER,       /* copy: the SE holds a different seed, not replaced */
    SEBK_E_NOSEED,      /* copy: no seed is open */
    SEBK_E_VERIFY,      /* copy: what was written did not read back and open */
    SEBK_E_VAULT_FULL,  /* restore: every vault slot is taken */
    SEBK_E_VAULT,       /* restore: the vault refused (locked, unreadable) */
} SeBackupResult;

/* What the container holds, as the Info screen says it. */
typedef enum {
    SEBK_STATE_EMPTY = 0,
    SEBK_STATE_COPY,        /* a LeekWallet seed copy (opening it needs the PIN) */
    SEBK_STATE_FOREIGN,     /* someone else's data; erase it to use the SE */
    SEBK_STATE_DAMAGED,     /* ours, unreadable: a cut write or decay */
} SeBackupState;

typedef struct {
    SeBackupState state;
    bool opened;                /* the PIN opened the copy; fields below valid */
    bool matches_open;          /* ...and it is the open seed's phrase */
    char zfp[9];                /* short fingerprint, "1A2B3C4D" */
    uint32_t birthday;
    uint32_t account;
    char label[ZKV_LABEL_MAX + 1];
} SeBackupPeek;

/** Is the chip there, and what does the container hold? No PIN, no KDF. */
SeBackupResult sebk_status(SeBackupState *state);

/**
 * Read the copy and, if it opens with `pin`, describe it (never the phrase).
 * SEBK_OK with `out->opened` false means there is no copy to open (state
 * says why); SEBK_E_PIN means there is one and this PIN does not open it.
 */
SeBackupResult sebk_peek(const char *pin, SeBackupPeek *out);

/**
 * Seal the open seed (stored or temporary) under `pin`, store it, then read
 * it back and open it before reporting success. Refused with SEBK_E_FOREIGN
 * if the object holds someone else's data, and with SEBK_E_OTHER if it holds
 * a copy of a different seed (or one this PIN cannot open) unless
 * `replace_other`. `zfp` (optional) gets the copy's short fingerprint.
 */
SeBackupResult sebk_copy_open_seed(const char *pin, const char *label, uint32_t birthday,
                                   uint32_t account, bool replace_other, char zfp[9]);

/** The same for a seed that is not the open one: an SD file's contents. */
SeBackupResult sebk_copy_contents(const char *pin, const ZkvContents *c, bool replace_other,
                                  char zfp[9]);

/** Open the copy and make it the session's temporary seed, as an SD load does. */
SeBackupResult sebk_restore_session(const char *pin, SeBackupPeek *out);

/**
 * Open the copy and store it in the vault under the backup rule
 * (wallet_store_mnemonic_if_new): a slot that already holds it is a no-op
 * (`*already`), otherwise a new slot after the last. Never overwrites or
 * deletes a vault seed.
 */
SeBackupResult sebk_restore_vault(const char *pin, SeBackupPeek *out, uint8_t *slot,
                                  bool *already);

/** Zero the whole container. The vault and the SD card are not touched. */
SeBackupResult sebk_erase(void);

const char *sebk_strerror(SeBackupResult r);
const char *sebk_state_name(SeBackupState s);

/* ------------------------------------------------- the format, pure */

/** Seal `c` with its fingerprint `fp` under `pin`. ZKV_OK, or ZKV_E_ARG. */
int sebk_seal(const char *pin, size_t pin_len, const ZkvContents *c, const uint8_t fp[32],
              uint8_t *out, size_t cap, size_t *out_len);

/**
 * Open a blob. ZKV_E_FORMAT if it is not an LKSV v1 blob (the container then
 * holds someone else's data), ZKV_E_KDF for a work factor outside the
 * accepted range, ZKV_E_AUTH for a PIN that does not open it or an altered
 * blob, ZKV_E_PAYLOAD for an authentic but malformed one. `c` and `fp` are
 * zeroed on any failure.
 */
int sebk_open(const uint8_t *blob, size_t len, const char *pin, size_t pin_len, ZkvContents *c,
              uint8_t fp[32]);

/* ------------------------------------ the container, as this file needs it */

/* Implemented by se-optiga.c on the board (leek_se_container_*), and by a
 * fake in the simulator. Same states as optiga_container_state_t. */
typedef enum {
    SEBK_OBJ_EMPTY = 0,
    SEBK_OBJ_OK,
    SEBK_OBJ_INCOMPLETE,
    SEBK_OBJ_DAMAGED,
    SEBK_OBJ_FOREIGN,
} SeBackupObj;

#define SEBK_STORE_OK        0
#define SEBK_STORE_E_IO     (-1)
#define SEBK_STORE_E_BIG    (-2)    /* read: a payload larger than `cap` */
#define SEBK_STORE_E_FOREIGN (-3)   /* write: refused, the object is not a container */
#define SEBK_STORE_E_VERIFY (-4)    /* write: the chip's read-back differed */

bool sebk_store_present(void);
int sebk_store_read(uint8_t *out, size_t cap, size_t *len, SeBackupObj *state);
int sebk_store_write(const uint8_t *data, size_t len);
int sebk_store_erase(void);

#endif /* LEEK_SE_BACKUP_H */
