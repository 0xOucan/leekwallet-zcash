/**
 * The .zkv SD seed file. See zkv.h.
 */
#include "zkv.h"

#include <string.h>

#include "aes/aesgcm.h"
#include "memzero.h"
#include "pbkdf2.h"
#include "rand.h"

#define OFF_MAGIC   0
#define OFF_VERSION 4
#define OFF_KDF     5
#define OFF_ITER    6
#define OFF_SALT    10
#define OFF_NONCE   26
#define SALT_SIZE   16
#define NONCE_SIZE  12

/* Prefixed to the file's salt: a .zkv key can never coincide with a key
 * derived for anything else from the same passphrase. */
static const char KDF_DOMAIN[] = "LeekWallet zkv v1";

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

bool zkv_pass_ok(const char *pass, size_t pass_len)
{
    if (!pass || pass_len < ZKV_PASS_MIN || pass_len > ZKV_PASS_MAX) {
        return false;
    }
    for (size_t i = 0; i < pass_len; i++) {
        if (pass[i] < 0x20 || pass[i] > 0x7E) {
            return false;
        }
    }
    return true;
}

static bool label_ok(const char *label, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (label[i] < 0x20 || label[i] > 0x7E) {
            return false;
        }
    }
    return true;
}

static bool entropy_len_ok(uint8_t n)
{
    return n >= 16 && n <= 32 && n % 4 == 0;
}

static void derive_key(const char *pass, size_t pass_len, uint32_t iterations,
                       const uint8_t salt[SALT_SIZE], uint8_t key[32])
{
    uint8_t s[sizeof KDF_DOMAIN - 1 + SALT_SIZE];
    memcpy(s, KDF_DOMAIN, sizeof KDF_DOMAIN - 1);
    memcpy(s + sizeof KDF_DOMAIN - 1, salt, SALT_SIZE);
    uint8_t full[64];
    pbkdf2_hmac_sha512((const uint8_t *)pass, (int)pass_len, s, (int)sizeof s, iterations,
                       full, sizeof full);
    memcpy(key, full, 32);
    memzero(full, sizeof full);
}

/* The payload's bytes; 0 if `c` cannot be represented. */
static size_t encode_payload(const ZkvContents *c, uint8_t *p)
{
    size_t label_len = strnlen(c->label, sizeof c->label);
    if (!entropy_len_ok(c->entropy_len) || c->account >= 0x80000000u ||
        (c->flags & ~ZKV_FLAG_USE_QSK) || label_len > ZKV_LABEL_MAX ||
        !label_ok(c->label, label_len)) {
        return 0;
    }
    size_t n = 0;
    p[n++] = 1;
    p[n++] = c->entropy_len;
    memcpy(p + n, c->entropy, c->entropy_len);
    n += c->entropy_len;
    put_u32(p + n, c->birthday);
    n += 4;
    put_u32(p + n, c->account);
    n += 4;
    p[n++] = c->flags;
    p[n++] = (uint8_t)label_len;
    memcpy(p + n, c->label, label_len);
    return n + label_len;
}

static int decode_payload(const uint8_t *p, size_t len, ZkvContents *c)
{
    if (len < 2 || p[0] != 1 || !entropy_len_ok(p[1])) return ZKV_E_PAYLOAD;
    size_t el = p[1];
    if (len < 2 + el + 10) return ZKV_E_PAYLOAD;
    size_t n = 2;
    c->entropy_len = (uint8_t)el;
    memcpy(c->entropy, p + n, el);
    n += el;
    c->birthday = get_u32(p + n);
    n += 4;
    c->account = get_u32(p + n);
    n += 4;
    c->flags = p[n++];
    size_t ll = p[n++];
    /* Exactly the declared length: trailing bytes are as wrong as missing
     * ones, and authentic does not mean well-formed. */
    if (c->account >= 0x80000000u || (c->flags & ~ZKV_FLAG_USE_QSK) || ll > ZKV_LABEL_MAX ||
        n + ll != len || !label_ok((const char *)p + n, ll)) {
        return ZKV_E_PAYLOAD;
    }
    memcpy(c->label, p + n, ll);
    c->label[ll] = '\0';
    return ZKV_OK;
}

static int seal(const ZkvContents *c, const char *pass, size_t pass_len, uint32_t iterations,
                const uint8_t salt[SALT_SIZE], const uint8_t nonce[NONCE_SIZE],
                uint8_t *out, size_t cap, size_t *out_len)
{
    if (!c || !out || !out_len) return ZKV_E_ARG;
    if (!zkv_pass_ok(pass, pass_len)) return ZKV_E_PASS;
    if (iterations < ZKV_ITERATIONS_MIN || iterations > ZKV_ITERATIONS_MAX) return ZKV_E_KDF;

    uint8_t payload[ZKV_PAYLOAD_MAX];
    size_t plen = encode_payload(c, payload);
    if (plen == 0) return ZKV_E_ARG;
    size_t total = ZKV_HEADER_SIZE + plen + ZKV_TAG_SIZE;
    if (cap < total) {
        memzero(payload, sizeof payload);
        return ZKV_E_ARG;
    }

    memcpy(out + OFF_MAGIC, ZKV_MAGIC, 4);
    out[OFF_VERSION] = ZKV_VERSION;
    out[OFF_KDF] = ZKV_KDF_PBKDF2_SHA512;
    put_u32(out + OFF_ITER, iterations);
    memcpy(out + OFF_SALT, salt, SALT_SIZE);
    memcpy(out + OFF_NONCE, nonce, NONCE_SIZE);
    memcpy(out + ZKV_HEADER_SIZE, payload, plen);
    memzero(payload, sizeof payload);

    uint8_t key[32];
    derive_key(pass, pass_len, iterations, salt, key);
    gcm_ctx ctx[1];
    int rc = ZKV_E_ARG;
    if (gcm_init_and_key(key, 32, ctx) == RETURN_GOOD &&
        gcm_encrypt_message(out + OFF_NONCE, NONCE_SIZE, out, ZKV_HEADER_SIZE,
                            out + ZKV_HEADER_SIZE, (unsigned long)plen,
                            out + ZKV_HEADER_SIZE + plen, ZKV_TAG_SIZE, ctx) == RETURN_GOOD) {
        rc = ZKV_OK;
        *out_len = total;
    }
    gcm_end(ctx);
    memzero(ctx, sizeof ctx);
    memzero(key, sizeof key);
    if (rc != ZKV_OK) memzero(out, total);
    return rc;
}

int zkv_seal(const ZkvContents *c, const char *pass, size_t pass_len, uint32_t iterations,
             uint8_t *out, size_t cap, size_t *out_len)
{
    uint8_t salt[SALT_SIZE], nonce[NONCE_SIZE];
    /* Fresh for every file: each seal also has a fresh key, so the nonce is
     * belt and braces, never the only thing between two files and reuse. */
    random_buffer(salt, sizeof salt);
    random_buffer(nonce, sizeof nonce);
    return seal(c, pass, pass_len, iterations, salt, nonce, out, cap, out_len);
}

int zkv_peek(const uint8_t *file, size_t len, uint32_t *iterations)
{
    if (!file) return ZKV_E_ARG;
    if (len < ZKV_HEADER_SIZE + ZKV_TAG_SIZE || memcmp(file + OFF_MAGIC, ZKV_MAGIC, 4) != 0) {
        return ZKV_E_FORMAT;
    }
    if (len > ZKV_FILE_MAX) return ZKV_E_FORMAT;
    if (file[OFF_VERSION] != ZKV_VERSION || file[OFF_KDF] != ZKV_KDF_PBKDF2_SHA512) {
        return ZKV_E_VERSION;
    }
    uint32_t it = get_u32(file + OFF_ITER);
    if (it < ZKV_ITERATIONS_MIN || it > ZKV_ITERATIONS_MAX) return ZKV_E_KDF;
    if (iterations) *iterations = it;
    return ZKV_OK;
}

int zkv_open(const uint8_t *file, size_t len, const char *pass, size_t pass_len,
             ZkvContents *out)
{
    if (!out) return ZKV_E_ARG;
    memzero(out, sizeof *out);
    uint32_t iterations = 0;
    int rc = zkv_peek(file, len, &iterations);
    if (rc != ZKV_OK) return rc;
    /* A passphrase that could never have sealed a file is not worth a KDF. */
    if (!zkv_pass_ok(pass, pass_len)) return ZKV_E_AUTH;

    size_t plen = len - ZKV_HEADER_SIZE - ZKV_TAG_SIZE;
    uint8_t payload[ZKV_PAYLOAD_MAX];
    memcpy(payload, file + ZKV_HEADER_SIZE, plen);

    uint8_t key[32];
    derive_key(pass, pass_len, iterations, file + OFF_SALT, key);
    gcm_ctx ctx[1];
    rc = ZKV_E_AUTH;
    if (gcm_init_and_key(key, 32, ctx) == RETURN_GOOD &&
        gcm_decrypt_message(file + OFF_NONCE, NONCE_SIZE, file, ZKV_HEADER_SIZE, payload,
                            (unsigned long)plen, file + ZKV_HEADER_SIZE + plen, ZKV_TAG_SIZE,
                            ctx) == RETURN_GOOD) {
        rc = decode_payload(payload, plen, out);
    }
    gcm_end(ctx);
    memzero(ctx, sizeof ctx);
    memzero(key, sizeof key);
    memzero(payload, sizeof payload);
    if (rc != ZKV_OK) memzero(out, sizeof *out);
    return rc;
}

const char *zkv_strerror(int code)
{
    switch (code) {
        case ZKV_OK:        return "ok";
        case ZKV_E_ARG:     return "invalid contents";
        case ZKV_E_FORMAT:  return "not a seed file";
        case ZKV_E_VERSION: return "unknown file version";
        case ZKV_E_KDF:     return "unsupported work factor";
        case ZKV_E_AUTH:    return "wrong passphrase or damaged file";
        case ZKV_E_PAYLOAD: return "malformed contents";
        case ZKV_E_PASS:    return "passphrase must be 8-64 printable ASCII";
        default:            return "unknown error";
    }
}

#ifdef LEEK_HOST_TEST
int zkv__seal_fixed_for_test(const ZkvContents *c, const char *pass, size_t pass_len,
                             uint32_t iterations, const uint8_t salt[16],
                             const uint8_t nonce[12], uint8_t *out, size_t cap,
                             size_t *out_len)
{
    return seal(c, pass, pass_len, iterations, salt, nonce, out, cap, out_len);
}
#endif
