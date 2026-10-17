/**
 * PCZT intake and signing. See zcash-sign.h.
 */
#include "zcash-sign.h"

#include <stdlib.h>
#include <string.h>

#include "leek-wallet.h"
#include "memzero.h"
#include "sha2.h"
#include "zcash-worker.h"
#include "zsign_c.h"
#include "zsign_rust.h"

#ifndef LEEK_HOST_TEST
#include "esp_heap_caps.h"
#endif

/* ------------------------------------------------------------- memory */

/* The PCZT is public (it is what the host built), so PSRAM is the right
 * place for it: the 512 KB bound is far more than internal RAM can spare.
 * QEMU and boards without PSRAM fall back to internal memory. Seeds never
 * come from here (zsign uses its own heap, see zsign-rust/platform). */
static void *big_alloc(size_t n)
{
#ifdef LEEK_HOST_TEST
    return malloc(n);
#else
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : heap_caps_malloc(n, MALLOC_CAP_8BIT);
#endif
}

static void big_free(void *p)
{
#ifdef LEEK_HOST_TEST
    free(p);
#else
    heap_caps_free(p);
#endif
}

/* -------------------------------------------------------------- state */

typedef enum { ST_IDLE, ST_RECEIVING, ST_CHECKED } State;

/* Two summaries are ~25 KB, so they live in the same big allocation as the
 * bytes rather than in internal .bss. */
typedef struct {
    zs_summary rust;
    zs_summary c;
} Summaries;

static struct {
    State     st;
    uint8_t  *buf;
    uint32_t  len, got;
    uint8_t   sha256[32];
    uint32_t  account;
    uint8_t   network;
    Summaries *sums;
    const zs_summary *agreed;
    zs_ctx   *rctx;
    zc_ctx   *cctx;
} S;

/*
 * The device runs the C backend; the host suites and zdev run BOTH.
 *
 * Measured on the first S3 board at 160 MHz: deriving one Orchard account
 * took 0.75 s in C and 97 s in Rust. pasta_curves works in 64-bit limbs with
 * 128-bit products, which the 32-bit Xtensa core emulates, and Orchard's
 * ZIP 32 path validates every intermediate key with full scalar
 * multiplications. A check-and-sign in Rust would hold the device for
 * minutes. So BOTH stays the reference everywhere it is affordable: the two
 * backends agree on every key and PCZT vector and on 3,675 mutations in
 * test_zsign_diff, and the C backend is the one that runs on the chip.
 * LEEK_ZCASH_BACKEND_BOTH forces BOTH on a device build, for measurements.
 */
#if defined(LEEK_HAS_ZCASH_C_ONLY)
static ZcashSignBackend backend = ZSIGN_BACKEND_C;
#elif defined(LEEK_HOST_TEST) || defined(LEEK_ZCASH_BACKEND_BOTH)
static ZcashSignBackend backend = ZSIGN_BACKEND_BOTH;
#else
static ZcashSignBackend backend = ZSIGN_BACKEND_C;
#endif

#ifdef LEEK_HOST_TEST
static bool inject_disagreement;
void zcash_sign__inject_disagreement_for_test(bool on) { inject_disagreement = on; }
#endif

void zcash_sign_set_backend(ZcashSignBackend b)
{
    if (b <= ZSIGN_BACKEND_BOTH) backend = b;
}

ZcashSignBackend zcash_sign_backend(void) { return backend; }

const char *zcash_sign_backend_name(ZcashSignBackend b)
{
    switch (b) {
        case ZSIGN_BACKEND_RUST: return "rust";
        case ZSIGN_BACKEND_C:    return "c";
        case ZSIGN_BACKEND_BOTH: return "both";
    }
    return "?";
}

static bool use_rust(void) { return backend != ZSIGN_BACKEND_C; }
static bool use_c(void)    { return backend != ZSIGN_BACKEND_RUST; }

static void free_contexts(void)
{
    if (S.rctx) { zs_pczt_free(S.rctx); S.rctx = NULL; }
    if (S.cctx) { zc_pczt_free(S.cctx); S.cctx = NULL; }
}

void zcash_sign_reset(void)
{
    free_contexts();
    if (S.buf) big_free(S.buf);
    if (S.sums) {
        memzero(S.sums, sizeof *S.sums);
        big_free(S.sums);
    }
    memset(&S, 0, sizeof S);
}

/* ------------------------------------------------------------- intake */

int zcash_sign_begin(uint32_t len, const uint8_t sha256[32], uint32_t account, uint8_t network)
{
    zcash_sign_reset();
    if (!sha256 || account >= 0x80000000u || (network != ZS_NET_MAIN && network != ZS_NET_TEST)) {
        return ZS_ERR_ARG;
    }
    if (len == 0 || len > ZCASH_PCZT_MAX) {
        return ZSIGN_E_TOO_LARGE;
    }
    S.buf = big_alloc(len);
    S.sums = big_alloc(sizeof *S.sums);
    if (!S.buf || !S.sums) {
        zcash_sign_reset();
        return ZSIGN_E_NOMEM;
    }
    memset(S.sums, 0, sizeof *S.sums);
    S.len = len;
    S.got = 0;
    memcpy(S.sha256, sha256, 32);
    S.account = account;
    S.network = network;
    S.st = ST_RECEIVING;
    return ZS_OK;
}

int zcash_sign_chunk(uint32_t offset, const uint8_t *data, size_t n)
{
    if (S.st != ST_RECEIVING) return ZSIGN_E_STATE;
    if (!data || n == 0 || offset != S.got || n > S.len - S.got) {
        /* Out of order or past the end: the upload is no longer the one the
         * host declared, so it goes. */
        zcash_sign_reset();
        return ZSIGN_E_OFFSET;
    }
    memcpy(S.buf + S.got, data, n);
    S.got += (uint32_t)n;
    return ZS_OK;
}

uint8_t zcash_sign_network(void)
{
    return S.network;
}

uint32_t zcash_sign_received(void)
{
    return S.st == ST_RECEIVING ? S.got : 0;
}

/* -------------------------------------------------------------- check */

typedef struct {
    int rc;
} CheckJob;

static WalletError check_with_seed(const uint8_t seed[64], void *p)
{
    CheckJob *job = p;
    int32_t err = 0;

    if (use_rust()) {
        S.rctx = zs_pczt_new(S.buf, S.len, &err);
        if (!S.rctx) { job->rc = err; return WALLET_OK; }
        job->rc = zs_pczt_check(S.rctx, seed, S.account, S.network, &S.sums->rust);
        if (job->rc != ZS_OK) return WALLET_OK;
    }
    if (use_c()) {
        S.cctx = zc_pczt_new(S.buf, S.len, &err);
        int32_t rc = S.cctx ? zc_pczt_check(S.cctx, seed, S.account, S.network, &S.sums->c) : err;
        if (backend == ZSIGN_BACKEND_BOTH && rc != ZS_OK) {
            /* Rust passed and C refused: that is a disagreement, whatever
             * C's reason. */
            job->rc = ZSIGN_E_DISAGREE;
            return WALLET_OK;
        }
        job->rc = rc;
    }
    return WALLET_OK;
}

static void check_job(void *p)
{
    CheckJob *job = p;
    if (wallet_with_seed(check_with_seed, job) != WALLET_OK && job->rc == ZS_OK) {
        job->rc = ZSIGN_E_LOCKED;
    }
}

int zcash_sign_check(const zs_summary **out)
{
    if (out) *out = NULL;
    if (S.st != ST_RECEIVING) return ZSIGN_E_STATE;
    if (S.got != S.len) { zcash_sign_reset(); return ZSIGN_E_INCOMPLETE; }

    uint8_t digest[32];
    sha256_Raw(S.buf, S.len, digest);
    if (memcmp(digest, S.sha256, 32) != 0) {
        zcash_sign_reset();
        return ZSIGN_E_HASH;
    }

    CheckJob job = { ZS_OK };
    if (!zcash_worker_run(check_job, &job)) job.rc = ZSIGN_E_WORKER;

#ifdef LEEK_HOST_TEST
    if (job.rc == ZS_OK && inject_disagreement && use_c()) S.sums->c.fee ^= 1;
#endif
    if (job.rc == ZS_OK && backend == ZSIGN_BACKEND_BOTH &&
        memcmp(&S.sums->rust, &S.sums->c, sizeof S.sums->rust) != 0) {
        job.rc = ZSIGN_E_DISAGREE;
    }
    if (job.rc != ZS_OK) {
        zcash_sign_reset();
        return job.rc;
    }
    S.agreed = use_rust() ? &S.sums->rust : &S.sums->c;
    S.st = ST_CHECKED;
    if (out) *out = S.agreed;
    return ZS_OK;
}

/* --------------------------------------------------------------- sign */

typedef struct {
    zs_sig  *sigs;
    uint32_t max, n;
    int      rc;
} SignJob;

static WalletError sign_with_seed(const uint8_t seed[64], void *p)
{
    SignJob *job = p;
    job->rc = use_rust()
        ? zs_pczt_sign(S.rctx, seed, S.account, S.network, job->sigs, job->max, &job->n)
        : zc_pczt_sign(S.cctx, seed, S.account, S.network, job->sigs, job->max, &job->n);
    return WALLET_OK;
}

static void sign_job(void *p)
{
    SignJob *job = p;
    if (wallet_with_seed(sign_with_seed, job) != WALLET_OK && job->rc == ZS_OK) {
        job->rc = ZSIGN_E_LOCKED;
        return;
    }
    if (job->rc != ZS_OK) return;
    /* Nothing leaves unverified: under BOTH, by the backend that did not
     * make it as well. */
    for (uint32_t i = 0; i < job->n; i++) {
        if ((use_rust() && zs_pczt_verify_sig(S.rctx, &job->sigs[i]) != ZS_OK) ||
            (use_c() && zc_pczt_verify_sig(S.cctx, &job->sigs[i]) != ZS_OK)) {
            job->rc = ZSIGN_E_VERIFY;
            return;
        }
    }
}

int zcash_sign_sign(zs_sig *sigs, uint32_t max, uint32_t *n_out)
{
    if (n_out) *n_out = 0;
    if (S.st != ST_CHECKED) return ZSIGN_E_STATE;
    if (!sigs || !n_out) { zcash_sign_reset(); return ZS_ERR_ARG; }

    SignJob job = { sigs, max, 0, ZS_OK };
    if (!zcash_worker_run(sign_job, &job)) job.rc = ZSIGN_E_WORKER;
    zcash_sign_reset();
    if (job.rc != ZS_OK) {
        memzero(sigs, sizeof *sigs * max);
        return job.rc;
    }
    *n_out = job.n;
    return ZS_OK;
}

/* --------------------------------------------------------- encodings */

/* One DER INTEGER from a 32-byte big-endian unsigned value: leading zeros
 * dropped, a 0x00 added when the top bit is set. */
static size_t der_int(const uint8_t v[32], uint8_t *out)
{
    int i = 0;
    while (i < 31 && v[i] == 0) i++;
    size_t n = (size_t)(32 - i), pad = (v[i] & 0x80) ? 1 : 0;
    out[0] = 0x02;
    out[1] = (uint8_t)(n + pad);
    out[2] = 0x00;
    memcpy(out + 2 + pad, v + i, n);
    return 2 + pad + n;
}

size_t zcash_tsig_encode(const zs_sig *s, uint8_t out[ZCASH_TSIG_MAX])
{
    if (!s || s->pool != ZS_POOL_TRANSPARENT) return 0;
    uint8_t body[70];
    size_t n = der_int(s->sig, body);
    n += der_int(s->sig + 32, body + n);
    out[0] = 0x30;
    out[1] = (uint8_t)n;
    memcpy(out + 2, body, n);
    out[2 + n] = 0x01;                  /* SIGHASH_ALL */
    return n + 3;
}

/* ------------------------------------------------------------ strings */

const char *zcash_sign_strerror(int code)
{
    switch (code) {
        case ZS_OK:                   return "ok";
        case ZS_ERR_ARG:              return "bad argument";
        case ZS_E_PARSE:              return "not a PCZT this device can read";
        case ZS_E_UNSUPPORTED_BRANCH: return "unsupported network upgrade";
        case ZS_E_SPEND_NOT_OURS:     return "spends funds this wallet does not own";
        case ZS_E_OUTPUT_COMMITMENT:  return "an output does not match its commitment";
        case ZS_E_USER_ADDRESS:       return "a shown address does not match its output";
        case ZS_E_VALUE_BALANCE:      return "values do not balance";
        case ZS_E_UNSUPPORTED_BUNDLE: return "Sapling, P2SH or a sighash type the device does not sign";
        case ZS_E_NETWORK:            return "wrong network";
        case ZS_E_TOO_MANY:           return "too many actions";
        case ZS_E_MISSING_DATA:       return "PCZT is missing data the device must check";
        case ZS_E_DUMMY_KEY:          return "dummy spend key present";
        case ZS_E_NOT_CHECKED:        return "not checked";
        case ZS_E_SIGN:               return "signing failed";
        case ZS_E_INTERNAL:           return "internal error";
        case ZSIGN_E_STATE:           return "no upload in progress";
        case ZSIGN_E_TOO_LARGE:       return "PCZT too large";
        case ZSIGN_E_OFFSET:          return "chunk out of order";
        case ZSIGN_E_HASH:            return "PCZT does not match its hash";
        case ZSIGN_E_INCOMPLETE:      return "upload incomplete";
        case ZSIGN_E_DISAGREE:        return "signer backends disagree";
        case ZSIGN_E_NOMEM:           return "out of memory";
        case ZSIGN_E_WORKER:          return "Zcash worker unavailable";
        case ZSIGN_E_LOCKED:          return "device is locked";
        case ZSIGN_E_VERIFY:          return "signature failed verification";
        default:                      return "refused";
    }
}
