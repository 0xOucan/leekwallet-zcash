/**
 * The seed's copy in the SE container. See se-backup.h.
 */
#include "se-backup.h"

#include "board.h"

#if LEEK_HAS_SE && LEEK_SE_CONTAINER_WRITES

#include <stdio.h>
#include <string.h>

#include "bip39.h"
#include "leek-wallet.h"
#include "memzero.h"
#include "rand.h"
#include "zcash-fp.h"

#define OFF_MAGIC   0
#define OFF_VERSION 4
#define OFF_PARAMS  5
#define OFF_SALT    (OFF_PARAMS + VAULT_PARAMS_BLOB_SIZE)
#define PLAIN_VERSION 1

/* Long enough for 24 words of the English list. */
#define PHRASE_MAX 256

/* ------------------------------------------------------------ the format */

int sebk_seal(const char *pin, size_t pin_len, const ZkvContents *c, const uint8_t fp[32],
              uint8_t *out, size_t cap, size_t *out_len)
{
    if (!pin || pin_len == 0 || !c || !fp || !out || !out_len) return ZKV_E_ARG;
    *out_len = 0;

    uint8_t plain[SEBK_PLAIN_MAX];
    plain[0] = PLAIN_VERSION;
    memcpy(plain + 1, fp, 32);
    size_t plen = zkv_payload_encode(c, plain + 33);
    if (plen == 0) {
        memzero(plain, sizeof plain);
        return ZKV_E_ARG;
    }
    plen += 33;
    const size_t total = SEBK_HEADER_SIZE + plen + VAULT_CRYPT_OVERHEAD;
    if (cap < total) {
        memzero(plain, sizeof plain);
        return ZKV_E_ARG;
    }

    /* The vault's parameters for a new vault, and a salt of the copy's own:
     * the same derivation as the vault's key, not the same key. */
    VaultKdfParams params;
    vault_params_default(VAULT_KDF_CURRENT, &params);
    memcpy(out + OFF_MAGIC, SEBK_MAGIC, 4);
    out[OFF_VERSION] = SEBK_VERSION;
    vault_params_serialize(&params, out + OFF_PARAMS, VAULT_PARAMS_BLOB_SIZE);
    random_buffer(out + OFF_SALT, VAULT_SALT_SIZE);

    uint8_t key[VAULT_KEY_SIZE];
    vault_derive_key_with(&params, pin, pin_len, out + OFF_SALT, key);
    size_t n = vault_encrypt(plain, plen, key, out + SEBK_HEADER_SIZE, cap - SEBK_HEADER_SIZE);
    memzero(key, sizeof key);
    memzero(plain, sizeof plain);
    if (n != plen + VAULT_CRYPT_OVERHEAD) {
        memzero(out, cap);
        return ZKV_E_ARG;
    }
    *out_len = total;
    return ZKV_OK;
}

int sebk_open(const uint8_t *blob, size_t len, const char *pin, size_t pin_len, ZkvContents *c,
              uint8_t fp[32])
{
    if (!c || !fp) return ZKV_E_ARG;
    memzero(c, sizeof *c);
    memzero(fp, 32);
    if (!blob || len < SEBK_HEADER_SIZE + VAULT_CRYPT_OVERHEAD + 34 || len > SEBK_BLOB_MAX ||
        memcmp(blob + OFF_MAGIC, SEBK_MAGIC, 4) != 0) {
        return ZKV_E_FORMAT;
    }
    if (blob[OFF_VERSION] != SEBK_VERSION) return ZKV_E_VERSION;
    /* The header is outside the AEAD (vault_encrypt() takes no associated
     * data), so it is held to its one canonical encoding: a reserved field
     * changed under the copy is refused here rather than ignored. */
    VaultKdfParams params;
    uint8_t canon[VAULT_PARAMS_BLOB_SIZE];
    if (!vault_params_parse(blob + OFF_PARAMS, VAULT_PARAMS_BLOB_SIZE, VAULT_KDF_CURRENT,
                            &params) ||
        params.family != VAULT_KDF_FAMILY_PBKDF2_SHA512 || params.iterations < SEBK_ITER_MIN ||
        params.iterations > SEBK_ITER_MAX || params.mem_kib || params.time_cost ||
        params.parallelism ||
        vault_params_serialize(&params, canon, sizeof canon) != VAULT_PARAMS_BLOB_SIZE ||
        memcmp(canon, blob + OFF_PARAMS, VAULT_PARAMS_BLOB_SIZE) != 0) {
        return ZKV_E_KDF;
    }
    if (!pin || pin_len == 0) return ZKV_E_AUTH;

    uint8_t key[VAULT_KEY_SIZE];
    vault_derive_key_with(&params, pin, pin_len, blob + OFF_SALT, key);
    uint8_t plain[SEBK_PLAIN_MAX];
    size_t plen = vault_decrypt(blob + SEBK_HEADER_SIZE, len - SEBK_HEADER_SIZE, key, plain,
                                sizeof plain);
    memzero(key, sizeof key);
    int rc = ZKV_E_AUTH;
    if (plen > 0) {
        rc = ZKV_E_PAYLOAD;
        if (plen > 33 && plain[0] == PLAIN_VERSION &&
            zkv_payload_decode(plain + 33, plen - 33, c) == ZKV_OK) {
            memcpy(fp, plain + 1, 32);
            rc = ZKV_OK;
        }
    }
    memzero(plain, sizeof plain);
    if (rc != ZKV_OK) {
        memzero(c, sizeof *c);
        memzero(fp, 32);
    }
    return rc;
}

/* ------------------------------------------------------- the container */

/* Read and classify. `blob`/`*len` hold an LKSV candidate only for
 * SEBK_STATE_COPY. */
static SeBackupResult read_copy(uint8_t blob[SEBK_BLOB_MAX], size_t *len, SeBackupState *state)
{
    *len = 0;
    *state = SEBK_STATE_EMPTY;
    if (!sebk_store_present()) return SEBK_E_NOCHIP;
    SeBackupObj obj = SEBK_OBJ_EMPTY;
    int rc = sebk_store_read(blob, SEBK_BLOB_MAX, len, &obj);
    if (rc == SEBK_STORE_E_BIG) {
        /* A container, but larger than any copy this firmware writes. */
        *len = 0;
        *state = SEBK_STATE_FOREIGN;
        return SEBK_OK;
    }
    if (rc != SEBK_STORE_OK) {
        *len = 0;
        return SEBK_E_IO;
    }
    switch (obj) {
        case SEBK_OBJ_EMPTY:      *state = SEBK_STATE_EMPTY; break;
        case SEBK_OBJ_INCOMPLETE:
        case SEBK_OBJ_DAMAGED:    *state = SEBK_STATE_DAMAGED; break;
        case SEBK_OBJ_FOREIGN:    *state = SEBK_STATE_FOREIGN; break;
        case SEBK_OBJ_OK:
            /* Our container; is the payload our copy? Only the magic and the
             * version are checked here: the rest needs the PIN. */
            *state = (*len >= SEBK_HEADER_SIZE && memcmp(blob, SEBK_MAGIC, 4) == 0)
                         ? (blob[OFF_VERSION] == SEBK_VERSION ? SEBK_STATE_COPY
                                                              : SEBK_STATE_FOREIGN)
                         : SEBK_STATE_FOREIGN;
            break;
        default:                  *state = SEBK_STATE_DAMAGED; break;
    }
    if (*state != SEBK_STATE_COPY) {
        memzero(blob, SEBK_BLOB_MAX);
        *len = 0;
    }
    return SEBK_OK;
}

static SeBackupResult from_open(int rc)
{
    switch (rc) {
        case ZKV_OK:      return SEBK_OK;
        case ZKV_E_AUTH:  return SEBK_E_PIN;
        case ZKV_E_FORMAT:
        case ZKV_E_VERSION:
            return SEBK_E_FOREIGN;
        default:          return SEBK_E_DAMAGED;
    }
}

/* The open seed's entropy, for "is the copy this seed?". */
static bool open_seed_entropy(ZkvContents *c)
{
    memzero(c, sizeof *c);
    char phrase[PHRASE_MAX];
    bool ok = wallet_get_mnemonic(phrase, sizeof phrase) == WALLET_OK &&
              zkv_entropy_from_phrase(phrase, c);
    memzero(phrase, sizeof phrase);
    return ok;
}

static bool same_entropy(const ZkvContents *a, const ZkvContents *b)
{
    if (a->entropy_len != b->entropy_len || a->entropy_len == 0) return false;
    uint8_t d = 0;
    for (size_t i = 0; i < a->entropy_len; i++) d |= (uint8_t)(a->entropy[i] ^ b->entropy[i]);
    return d == 0;
}

static void describe(const ZkvContents *c, const uint8_t fp[32], SeBackupPeek *out)
{
    out->opened = true;
    zcash_short_fp(fp, out->zfp);
    out->birthday = c->birthday;
    out->account = c->account;
    snprintf(out->label, sizeof out->label, "%s", c->label);
    ZkvContents mine;
    out->matches_open = open_seed_entropy(&mine) && same_entropy(&mine, c);
    memzero(&mine, sizeof mine);
}

/* Read the container and open the copy: `c`/`fp` valid only on SEBK_OK. */
static SeBackupResult open_copy(const char *pin, SeBackupPeek *peek, ZkvContents *c,
                                uint8_t fp[32])
{
    memset(peek, 0, sizeof *peek);
    memzero(c, sizeof *c);
    uint8_t blob[SEBK_BLOB_MAX];
    size_t len = 0;
    SeBackupResult r = read_copy(blob, &len, &peek->state);
    if (r == SEBK_OK) {
        switch (peek->state) {
            case SEBK_STATE_EMPTY:   r = SEBK_E_EMPTY; break;
            case SEBK_STATE_FOREIGN: r = SEBK_E_FOREIGN; break;
            case SEBK_STATE_DAMAGED: r = SEBK_E_DAMAGED; break;
            case SEBK_STATE_COPY:
                r = from_open(sebk_open(blob, len, pin, pin ? strlen(pin) : 0, c, fp));
                if (r == SEBK_OK) describe(c, fp, peek);
                break;
        }
    }
    memzero(blob, sizeof blob);
    return r;
}

/* --------------------------------------------------------------- the API */

SeBackupResult sebk_status(SeBackupState *state)
{
    uint8_t blob[SEBK_BLOB_MAX];
    size_t len = 0;
    SeBackupResult r = read_copy(blob, &len, state);
    memzero(blob, sizeof blob);
    return r;
}

SeBackupResult sebk_peek(const char *pin, SeBackupPeek *out)
{
    ZkvContents c;
    uint8_t fp[32];
    SeBackupResult r = open_copy(pin, out, &c, fp);
    memzero(&c, sizeof c);
    memzero(fp, sizeof fp);
    /* No copy is an answer, not a failure: out->state says which. */
    if (r == SEBK_E_EMPTY || r == SEBK_E_FOREIGN || r == SEBK_E_DAMAGED) return SEBK_OK;
    return r;
}

/* Is it safe to write over what the container holds now? */
static SeBackupResult may_replace(const char *pin, const ZkvContents *incoming, bool replace_other)
{
    SeBackupPeek peek;
    ZkvContents old;
    uint8_t fp[32];
    SeBackupResult r = open_copy(pin, &peek, &old, fp);
    const bool same = r == SEBK_OK && same_entropy(&old, incoming);
    memzero(&old, sizeof old);
    memzero(fp, sizeof fp);
    switch (r) {
        case SEBK_E_EMPTY:
        case SEBK_E_DAMAGED:
            return SEBK_OK;
        case SEBK_E_FOREIGN:
            /* Not ours to overwrite, whatever the caller says: Erase first. */
            return SEBK_E_FOREIGN;
        case SEBK_OK:
            return (same || replace_other) ? SEBK_OK : SEBK_E_OTHER;
        case SEBK_E_PIN:
            /* A copy under another PIN: as good as another seed. */
            return replace_other ? SEBK_OK : SEBK_E_OTHER;
        default:
            return r;
    }
}

SeBackupResult sebk_copy_contents(const char *pin, const ZkvContents *c, bool replace_other,
                                  char zfp[9])
{
    if (zfp) zfp[0] = '\0';
    if (!pin || !*pin || !c) return SEBK_E_NOSEED;

    const char *phrase = mnemonic_from_data(c->entropy, c->entropy_len);
    if (!phrase) return SEBK_E_NOSEED;
    uint8_t fp[32];
    zcash_fp_of_phrase(phrase, fp);
    mnemonic_clear();

    SeBackupResult r = may_replace(pin, c, replace_other);
    uint8_t blob[SEBK_BLOB_MAX];
    size_t len = 0;
    if (r == SEBK_OK && sebk_seal(pin, strlen(pin), c, fp, blob, sizeof blob, &len) != ZKV_OK) {
        r = SEBK_E_NOSEED;
    }
    if (r == SEBK_OK) {
        int w = sebk_store_write(blob, len);
        r = w == SEBK_STORE_OK        ? SEBK_OK
          : w == SEBK_STORE_E_FOREIGN ? SEBK_E_FOREIGN
          : w == SEBK_STORE_E_VERIFY  ? SEBK_E_VERIFY
                                      : SEBK_E_IO;
    }
    if (r == SEBK_OK) {
        /* The chip compared its bytes already; this proves the copy OPENS,
         * with this PIN, to this seed - what a backup has to do. */
        SeBackupPeek peek;
        ZkvContents back;
        uint8_t bfp[32];
        SeBackupResult o = open_copy(pin, &peek, &back, bfp);
        if (o != SEBK_OK || !same_entropy(&back, c) || memcmp(bfp, fp, 32) != 0) {
            r = SEBK_E_VERIFY;
        }
        memzero(&back, sizeof back);
        memzero(bfp, sizeof bfp);
    }
    if (r == SEBK_OK && zfp) zcash_short_fp(fp, zfp);
    memzero(blob, sizeof blob);
    memzero(fp, sizeof fp);
    return r;
}

SeBackupResult sebk_copy_open_seed(const char *pin, const char *label, uint32_t birthday,
                                   uint32_t account, bool replace_other, char zfp[9])
{
    if (zfp) zfp[0] = '\0';
    ZkvContents c;
    if (!open_seed_entropy(&c)) {
        memzero(&c, sizeof c);
        return SEBK_E_NOSEED;
    }
    c.birthday = birthday;
    c.account = account < 0x80000000u ? account : 0;
    /* Printable ASCII only, as the .zkv label: anything else is dropped. */
    size_t n = 0;
    for (const char *p = label ? label : ""; *p && n < ZKV_LABEL_MAX; p++) {
        if (*p >= 0x20 && *p <= 0x7E) c.label[n++] = *p;
    }
    c.label[n] = '\0';
    SeBackupResult r = sebk_copy_contents(pin, &c, replace_other, zfp);
    memzero(&c, sizeof c);
    return r;
}

SeBackupResult sebk_restore_session(const char *pin, SeBackupPeek *out)
{
    ZkvContents c;
    uint8_t fp[32];
    SeBackupResult r = open_copy(pin, out, &c, fp);
    if (r == SEBK_OK) {
        const char *phrase = mnemonic_from_data(c.entropy, c.entropy_len);
        if (!phrase || wallet_use_temporary_mnemonic(phrase) != WALLET_OK) r = SEBK_E_VAULT;
        mnemonic_clear();
        if (r == SEBK_OK) out->matches_open = true;
    }
    memzero(&c, sizeof c);
    memzero(fp, sizeof fp);
    return r;
}

SeBackupResult sebk_restore_vault(const char *pin, SeBackupPeek *out, uint8_t *slot,
                                  bool *already)
{
    *slot = 0;
    *already = false;
    ZkvContents c;
    uint8_t fp[32];
    SeBackupResult r = open_copy(pin, out, &c, fp);
    if (r == SEBK_OK) {
        const char *phrase = mnemonic_from_data(c.entropy, c.entropy_len);
        WalletError err = phrase ? wallet_store_mnemonic_if_new(phrase, slot, already)
                                 : WALLET_ERROR_INVALID_MNEMONIC;
        mnemonic_clear();
        r = err == WALLET_OK                  ? SEBK_OK
          : err == WALLET_ERROR_STORAGE_FULL  ? SEBK_E_VAULT_FULL
                                              : SEBK_E_VAULT;
    }
    memzero(&c, sizeof c);
    memzero(fp, sizeof fp);
    return r;
}

SeBackupResult sebk_erase(void)
{
    if (!sebk_store_present()) return SEBK_E_NOCHIP;
    return sebk_store_erase() == SEBK_STORE_OK ? SEBK_OK : SEBK_E_IO;
}

const char *sebk_strerror(SeBackupResult r)
{
    switch (r) {
        case SEBK_OK:           return "OK";
        case SEBK_E_NOCHIP:     return "No secure element";
        case SEBK_E_IO:         return "SE read/write error";
        case SEBK_E_EMPTY:      return "SE copy is empty";
        case SEBK_E_FOREIGN:    return "SE holds other data";
        case SEBK_E_DAMAGED:    return "SE copy is damaged";
        case SEBK_E_PIN:        return "PIN does not open it";
        case SEBK_E_OTHER:      return "SE has another seed";
        case SEBK_E_NOSEED:     return "No seed to copy";
        case SEBK_E_VERIFY:     return "Did not read back";
        case SEBK_E_VAULT_FULL: return "Vault is full";
        case SEBK_E_VAULT:      return "Vault refused it";
        default:                return "Error";
    }
}

const char *sebk_state_name(SeBackupState s)
{
    switch (s) {
        case SEBK_STATE_EMPTY:   return "empty";
        case SEBK_STATE_COPY:    return "LeekWallet copy";
        case SEBK_STATE_FOREIGN: return "other data";
        case SEBK_STATE_DAMAGED: return "damaged";
        default:                 return "?";
    }
}

#endif /* LEEK_HAS_SE && LEEK_SE_CONTAINER_WRITES */
