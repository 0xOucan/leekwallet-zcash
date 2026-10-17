/**
 * Per-account metadata. See zcash-meta.h.
 */
#include "zcash-meta.h"

#include <stdio.h>
#include <string.h>

#include "leek-wallet.h"
#include "memzero.h"
#include "nvs.h"
#include "zcash-fp.h"

#define NVS_NS "zc_meta"
#define BLOB_VERSION 1
/* NVS keys are at most 15 characters: "1A2B3C4D.99999" on testnet,
 * "1A2B3C4D:99999" on mainnet. Testnet keeps the original separator, so the
 * records written before networks were told apart (all testnet) still read. */
#define STORED_ACCOUNT_MAX 99999u
#define SESSION_SLOTS 8

typedef struct {
    bool      used;
    char      fp[ZCASH_FP_CHARS + 1];
    uint32_t  account;
    uint8_t   network;
    ZcashMeta meta;
} SessionRecord;

static SessionRecord session[SESSION_SLOTS];

static bool label_ok(const char *s)
{
    size_t n = strnlen(s, ZMETA_LABEL_MAX + 1);
    if (n > ZMETA_LABEL_MAX) return false;
    for (size_t i = 0; i < n; i++) {
        if (s[i] < 0x20 || s[i] > 0x7E) return false;
    }
    return true;
}

/* The open seed's short fingerprint, and whether its records may persist. */
static bool open_seed(char fp[ZCASH_FP_CHARS + 1], bool *persist)
{
    uint8_t full[32];
    if (zcash_seed_fingerprint(full) != WALLET_OK) return false;
    zcash_short_fp(full, fp);
    memzero(full, sizeof full);
    *persist = !wallet_has_passphrase() && !wallet_has_temporary_mnemonic();
    return true;
}

static SessionRecord *find_session(const char *fp, uint32_t account, uint8_t network)
{
    for (int i = 0; i < SESSION_SLOTS; i++) {
        if (session[i].used && session[i].account == account && session[i].network == network &&
            strcmp(session[i].fp, fp) == 0) {
            return &session[i];
        }
    }
    return NULL;
}

static void key_for(const char *fp, uint32_t account, uint8_t network, char key[16])
{
    snprintf(key, 16, "%.8s%c%u", fp, network == ZMETA_NET_MAIN ? ':' : '.',
             (unsigned)(account % (STORED_ACCOUNT_MAX + 1)));
}

static bool network_ok(uint8_t network)
{
    return network == ZMETA_NET_MAIN || network == ZMETA_NET_TEST;
}

bool zmeta_get(uint32_t account, uint8_t network, ZcashMeta *out)
{
    memset(out, 0, sizeof *out);
    if (!network_ok(network)) return false;
    char fp[ZCASH_FP_CHARS + 1];
    bool persist;
    if (!open_seed(fp, &persist)) return false;

    SessionRecord *r = find_session(fp, account, network);
    if (r) {
        *out = r->meta;
        return true;
    }
    if (!persist || account > STORED_ACCOUNT_MAX) return false;

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    char key[16];
    key_for(fp, account, network, key);
    uint8_t blob[7 + ZMETA_LABEL_MAX];
    size_t len = sizeof blob;
    esp_err_t err = nvs_get_blob(h, key, blob, &len);
    nvs_close(h);
    /* A record this firmware cannot read is no record: the birthday only
     * saves a question, and a wrong one would cost a missed note. */
    if (err != ESP_OK || len < 7 || blob[0] != BLOB_VERSION || blob[6] > ZMETA_LABEL_MAX ||
        len != 7u + blob[6]) {
        return false;
    }
    out->birthday = (uint32_t)blob[1] | (uint32_t)blob[2] << 8 | (uint32_t)blob[3] << 16 |
                    (uint32_t)blob[4] << 24;
    out->flags = blob[5];
    memcpy(out->label, blob + 7, blob[6]);
    out->label[blob[6]] = '\0';
    if (!label_ok(out->label) || (out->flags & ~ZMETA_FLAG_USE_QSK)) {
        memset(out, 0, sizeof *out);
        return false;
    }
    return true;
}

ZcashMetaResult zmeta_set(uint32_t account, uint8_t network, const ZcashMeta *m)
{
    if (!m || account >= 0x80000000u || !network_ok(network) || !label_ok(m->label) || (m->flags & ~ZMETA_FLAG_USE_QSK)) {
        return ZMETA_E_ARG;
    }
    char fp[ZCASH_FP_CHARS + 1];
    bool persist;
    if (!open_seed(fp, &persist)) return ZMETA_E_NO_SEED;

    if (!persist || account > STORED_ACCOUNT_MAX) {
        SessionRecord *r = find_session(fp, account, network);
        for (int i = 0; !r && i < SESSION_SLOTS; i++) {
            if (!session[i].used) r = &session[i];
        }
        if (!r) r = &session[SESSION_SLOTS - 1];   /* full: the newest wins */
        r->used = true;
        memcpy(r->fp, fp, sizeof r->fp);
        r->account = account;
        r->network = network;
        r->meta = *m;
        return ZMETA_OK_SESSION;
    }

    size_t ll = strlen(m->label);
    uint8_t blob[7 + ZMETA_LABEL_MAX];
    blob[0] = BLOB_VERSION;
    blob[1] = (uint8_t)m->birthday;
    blob[2] = (uint8_t)(m->birthday >> 8);
    blob[3] = (uint8_t)(m->birthday >> 16);
    blob[4] = (uint8_t)(m->birthday >> 24);
    blob[5] = m->flags;
    blob[6] = (uint8_t)ll;
    memcpy(blob + 7, m->label, ll);

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return ZMETA_E_NVS;
    char key[16];
    key_for(fp, account, network, key);
    esp_err_t err = nvs_set_blob(h, key, blob, 7 + ll);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK ? ZMETA_OK_STORED : ZMETA_E_NVS;
}

void zmeta_forget_session(void)
{
    memzero(session, sizeof session);
}

#ifdef LEEK_HOST_TEST
void zmeta__reset_for_test(void)
{
    zmeta_forget_session();
}
#endif
