/* LeekWallet Zcash: bounds-checked, allocation-free PCZT reader.
 *
 * Postcard wire format (postcard 1.x, https://postcard.jamesmunns.com):
 *   u8 / bool        one byte (bool must be 0 or 1)
 *   u16/u32/u64      LEB128 varint, at most 3/5/10 bytes, the last byte
 *                    bounded so the value fits; overlong forms are accepted
 *                    (postcard accepts them, and so must we to agree)
 *   i128             zigzag LEB128, at most 19 bytes
 *   usize lengths    varint of the platform's usize (as the Rust side)
 *   Option<T>        0x00 | 0x01 T
 *   Vec<T> / String  usize length, then elements; String must be UTF-8
 *   [u8; N], tuples  N raw bytes / fields in order, no length
 *   BTreeMap<K, V>   usize count, then (K, V) pairs
 *   enum             u32 varint variant index, then the variant's fields
 *   struct           fields in declaration order
 *
 * Layout (pczt 0.10.0-pre.2):
 *   v1: Global, transparent::Bundle, sapling::v1::Bundle, orchard::v1::Bundle
 *   v2: Global, Option<transparent::Bundle>, Option<sapling::Bundle>,
 *       Option<orchard::v2::Bundle> (Orchard), Option<orchard::v2::Bundle>
 *       (Ironwood)
 *
 * Unlike postcard::from_bytes, trailing bytes after the body are rejected:
 * the .zsig binds the hash of the exact bytes, so bytes the parser did not
 * read must not exist.
 */
#include "pczt_reader.h"

#include <string.h>

typedef struct {
    const uint8_t *p;
    size_t n;       /* bytes remaining */
    int err;
    int over_cap;   /* parsed fine so far, but more items than we store */
} rd;

/* Build with -DZC_READER_DEBUG to log where a parse stops (sim debugging). */
#ifdef ZC_READER_DEBUG
#include <stdio.h>
#define DBG(r) fprintf(stderr, "pczt_reader.c:%d: refused with %zu bytes left\n", __LINE__, (r)->n)
#else
#define DBG(r) ((void)0)
#endif
/* Record the first error only. */
#define SETERR(r, e) do { if (!(r)->err) { DBG(r); (r)->err = (e); } } while (0)
#define FAIL(r, e) do { SETERR(r, e); return 0; } while (0)
#define OK(r) (!(r)->err)

static const uint8_t *take(rd *r, size_t n)
{
    if (r->err) return NULL;
    if (n > r->n) { SETERR(r, ZC_R_PARSE); return NULL; }
    const uint8_t *p = r->p;
    r->p += n;
    r->n -= n;
    return p;
}

static uint8_t u8(rd *r)
{
    const uint8_t *p = take(r, 1);
    return p ? *p : 0;
}

/* Generic LEB128 varint for up to 64 bits, matching postcard's
 * try_take_varint_*: `maxb` bytes at most, last byte <= `lastmax`. */
static uint64_t varint(rd *r, unsigned maxb, uint8_t lastmax)
{
    uint64_t out = 0;
    for (unsigned i = 0; i < maxb; i++) {
        uint8_t b = u8(r);
        if (r->err) return 0;
        out |= (uint64_t)(b & 0x7F) << (7 * i);
        if ((b & 0x80) == 0) {
            if (i == maxb - 1 && b > lastmax) FAIL(r, ZC_R_PARSE);
            return out;
        }
    }
    FAIL(r, ZC_R_PARSE);
}

static uint32_t vu32(rd *r) { return (uint32_t)varint(r, 5, 0x0F); }
static uint64_t vu64(rd *r) { return varint(r, 10, 0x01); }

static uint64_t vusize(rd *r)
{
#if SIZE_MAX > 0xFFFFFFFFu
    return vu64(r);
#else
    return vu32(r);
#endif
}

/* i128 zigzag varint (19 bytes, last byte <= 0x03). Reports whether the
 * value fits an i64 and, if so, the value. */
static void vi128(rd *r, int64_t *v, uint8_t *fits)
{
    uint64_t lo = 0, hi = 0;
    *fits = 0;
    *v = 0;
    for (unsigned i = 0; i < 19; i++) {
        uint8_t b = u8(r);
        if (r->err) return;
        uint64_t c = b & 0x7F;
        unsigned sh = 7 * i;
        if (sh < 64) {
            lo |= c << sh;
            if (sh > 57) hi |= c >> (64 - sh);
        } else {
            hi |= c << (sh - 64);
        }
        if ((b & 0x80) == 0) {
            if (i == 18 && b > 0x03) { SETERR(r, ZC_R_PARSE); return; }
            if (hi == 0) {
                uint64_t mag = lo >> 1;
                *v = (lo & 1) ? -(int64_t)mag - 1 : (int64_t)mag;
                *fits = 1;
            }
            return;
        }
    }
    SETERR(r, ZC_R_PARSE);
}

/* Option tag: returns 1 for Some, 0 for None (or on error). */
static int opt(rd *r)
{
    uint8_t t = u8(r);
    if (r->err) return 0;
    if (t > 1) FAIL(r, ZC_R_PARSE);
    return t;
}

static uint8_t boolean(rd *r)
{
    uint8_t b = u8(r);
    if (!r->err && b > 1) SETERR(r, ZC_R_PARSE);
    return b;
}

static const uint8_t *arr(rd *r, size_t n) { return take(r, n); }

static const uint8_t *opt_arr(rd *r, size_t n)
{
    return opt(r) ? arr(r, n) : NULL;
}

/* Vec<u8>: also guards the length against remaining input before taking. */
static zc_bytes vec_u8(rd *r)
{
    zc_bytes b = { NULL, 0 };
    uint64_t n = vusize(r);
    if (r->err) return b;
    if (n > r->n) { SETERR(r, ZC_R_PARSE); return b; }
    b.p = take(r, (size_t)n);
    b.len = (uint32_t)n;
    return b;
}

int zc_utf8_valid(const uint8_t *s, size_t n)
{
    size_t i = 0;
    while (i < n) {
        uint8_t c = s[i];
        if (c < 0x80) { i++; continue; }
        size_t k;
        uint32_t cp, min;
        if ((c & 0xE0) == 0xC0) { k = 1; cp = c & 0x1F; min = 0x80; }
        else if ((c & 0xF0) == 0xE0) { k = 2; cp = c & 0x0F; min = 0x800; }
        else if ((c & 0xF8) == 0xF0) { k = 3; cp = c & 0x07; min = 0x10000; }
        else return 0;
        if (k > n - i - 1) return 0;     /* truncated sequence */
        for (size_t j = 1; j <= k; j++) {
            if ((s[i + j] & 0xC0) != 0x80) return 0;
            cp = (cp << 6) | (s[i + j] & 0x3F);
        }
        if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
        i += k + 1;
    }
    return 1;
}

static zc_bytes string(rd *r)
{
    zc_bytes b = vec_u8(r);
    if (OK(r) && !zc_utf8_valid(b.p, b.len)) SETERR(r, ZC_R_PARSE);
    return b;
}

static zc_bytes opt_string(rd *r)
{
    zc_bytes none = { NULL, 0 };
    return opt(r) ? string(r) : none;
}

static zc_bytes opt_vec(rd *r)
{
    zc_bytes none = { NULL, 0 };
    return opt(r) ? vec_u8(r) : none;
}

/* A count for a Vec/Map whose elements are each at least one byte, so a
 * count above the remaining input is malformed. Items past what we store
 * (actions, transparent outputs) are still parsed, into scratch, so that a
 * malformed PCZT is ZS_E_PARSE and only a well-formed one over the cap is
 * ZS_E_TOO_MANY, as the Rust parser would decide. */
#define NO_CAP UINT32_MAX
static uint32_t count(rd *r)
{
    uint64_t n = vusize(r);
    if (r->err) return 0;
    if (n > r->n) { SETERR(r, ZC_R_PARSE); return 0; }
    return (uint32_t)n;
}

/* BTreeMap<String, Vec<u8>> */
static void proprietary(rd *r)
{
    uint32_t n = count(r);
    for (uint32_t i = 0; i < n && OK(r); i++) {
        string(r);
        vec_u8(r);
    }
}

/* Zip32Derivation { seed_fingerprint: [u8; 32], derivation_path: Vec<u32> }.
 * Returns 1 if every index is hardened (what orchard's Zip32Derivation::parse
 * requires). */
static uint8_t zip32(rd *r)
{
    arr(r, 32);
    uint32_t n = count(r);
    uint8_t hardened = 1;
    for (uint32_t i = 0; i < n && OK(r); i++) {
        uint32_t idx = vu32(r);
        if (idx < 0x80000000u) hardened = 0;
    }
    return hardened;
}

/* BTreeMap<[u8; K], V> where V is Vec<u8> (deriv == 0) or Zip32Derivation. */
static void fixed_key_map(rd *r, size_t klen, int deriv)
{
    uint32_t n = count(r);
    for (uint32_t i = 0; i < n && OK(r); i++) {
        arr(r, klen);
        if (deriv) zip32(r); else vec_u8(r);
    }
}

/* Option<(u32, [[u8; 32]; 32])> */
static const uint8_t *witness(rd *r, uint32_t *pos)
{
    if (!opt(r)) return NULL;
    *pos = vu32(r);
    return arr(r, 32 * 32);
}

static void global(rd *r, zc_global *g)
{
    g->tx_version = vu32(r);
    g->version_group_id = vu32(r);
    g->consensus_branch_id = vu32(r);
    g->has_fallback_lock_time = (uint8_t)opt(r);
    if (g->has_fallback_lock_time) g->fallback_lock_time = vu32(r);
    g->expiry_height = vu32(r);
    g->coin_type = vu32(r);
    g->tx_modifiable = u8(r);
    proprietary(r);
}

static void t_input(rd *r, zc_pczt *z)
{
    arr(r, 32);                          /* prevout_txid */
    vu32(r);                             /* prevout_index */
    if (opt(r)) vu32(r);                 /* sequence */
    if (opt(r)) { vu32(r); z->tin_lock_time_set = 1; } /* required_time_lock_time */
    if (opt(r)) { vu32(r); z->tin_lock_time_set = 1; } /* required_height_lock_time */
    opt_vec(r);                          /* script_sig */
    vu64(r);                             /* value */
    vec_u8(r);                           /* script_pubkey */
    opt_vec(r);                          /* redeem_script */
    fixed_key_map(r, 33, 0);             /* partial_signatures */
    u8(r);                               /* sighash_type */
    fixed_key_map(r, 33, 1);             /* bip32_derivation */
    fixed_key_map(r, 20, 0);             /* ripemd160_preimages */
    fixed_key_map(r, 32, 0);             /* sha256_preimages */
    fixed_key_map(r, 20, 0);             /* hash160_preimages */
    fixed_key_map(r, 32, 0);             /* hash256_preimages */
    proprietary(r);
}

static void t_output(rd *r, zc_tout *o)
{
    o->value = vu64(r);
    o->script_pubkey = vec_u8(r);
    o->redeem_script = opt_vec(r);
    fixed_key_map(r, 33, 1);             /* bip32_derivation */
    o->user_address = opt_string(r);
    proprietary(r);
}

static void transparent(rd *r, zc_pczt *z)
{
    uint32_t n = count(r);
    z->n_tin = n > 0xFFFF ? 0xFFFF : (uint16_t)n;
    for (uint32_t i = 0; i < n && OK(r); i++) t_input(r, z);
    n = count(r);
    for (uint32_t i = 0; i < n && OK(r); i++) {
        static zc_tout scratch;
        if (z->n_tout < ZC_MAX_T_OUTPUTS) {
            t_output(r, &z->tout[z->n_tout++]);
        } else {
            r->over_cap = 1;
            t_output(r, &scratch);
        }
    }
}

#define GROTH_PROOF_SIZE 192

static void s_spend(rd *r)
{
    arr(r, 32); arr(r, 32); arr(r, 32);  /* cv, nullifier, rk */
    opt_arr(r, GROTH_PROOF_SIZE);        /* zkproof */
    opt_arr(r, 64);                      /* spend_auth_sig */
    opt_arr(r, 43);                      /* recipient */
    if (opt(r)) vu64(r);                 /* value */
    opt_arr(r, 32);                      /* rcm */
    opt_arr(r, 32);                      /* rseed */
    opt_arr(r, 32);                      /* rcv */
    if (opt(r)) { arr(r, 32); arr(r, 32); } /* proof_generation_key */
    uint32_t pos;
    witness(r, &pos);
    opt_arr(r, 32);                      /* alpha */
    if (opt(r)) zip32(r);                /* zip32_derivation */
    opt_arr(r, 32);                      /* dummy_ask */
    proprietary(r);
}

static void s_output(rd *r)
{
    arr(r, 32); arr(r, 32); arr(r, 32);  /* cv, cmu, ephemeral_key */
    vec_u8(r);                           /* enc_ciphertext */
    vec_u8(r);                           /* out_ciphertext */
    opt_arr(r, GROTH_PROOF_SIZE);        /* zkproof */
    opt_arr(r, 43);                      /* recipient */
    if (opt(r)) vu64(r);                 /* value */
    opt_arr(r, 32);                      /* rseed */
    opt_arr(r, 32);                      /* rcv */
    opt_arr(r, 32);                      /* ock */
    if (opt(r)) zip32(r);                /* zip32_derivation */
    opt_string(r);                       /* user_address */
    proprietary(r);
}

/* Sapling bundle; v1 carries a mandatory anchor, v2 an optional one. */
static void sapling(rd *r, zc_sapling *s, int v1)
{
    s->present = 1;
    uint32_t n = count(r);
    s->n_spends = n > 0xFFFF ? 0xFFFF : (uint16_t)n;
    for (uint32_t i = 0; i < n && OK(r); i++) s_spend(r);
    n = count(r);
    s->n_outputs = n > 0xFFFF ? 0xFFFF : (uint16_t)n;
    for (uint32_t i = 0; i < n && OK(r); i++) s_output(r);
    vi128(r, &s->value_sum, &s->value_sum_fits_i64);
    s->anchor = v1 ? arr(r, 32) : opt_arr(r, 32);
    s->bsk = opt_arr(r, 32);
}

static void o_action(rd *r, zc_action *a, int v1)
{
    memset(a, 0, sizeof *a);
    /* Action.cv_net */
    a->cv_net = v1 ? arr(r, 32) : opt_arr(r, 32);
    /* Spend */
    if (v1) {
        a->nullifier = arr(r, 32);
        a->rk = arr(r, 32);
    } else {
        /* v2 encodes both as Option, but into_logical requires them. */
        a->nullifier = opt_arr(r, 32);
        if (OK(r) && !a->nullifier) SETERR(r, ZC_R_PARSE);
        a->rk = opt_arr(r, 32);
        if (OK(r) && !a->rk) SETERR(r, ZC_R_PARSE);
    }
    a->spend_auth_sig = opt_arr(r, 64);
    a->s_recipient = opt_arr(r, 43);
    a->has_s_value = (uint8_t)opt(r);
    if (a->has_s_value) a->s_value = vu64(r);
    a->rho = opt_arr(r, 32);
    a->s_rseed = opt_arr(r, 32);
    a->fvk = opt_arr(r, 96);
    a->witness = witness(r, &a->witness_pos);
    a->alpha = opt_arr(r, 32);
    a->s_zip32_present = (uint8_t)opt(r);
    if (a->s_zip32_present) a->s_zip32_hardened = zip32(r);
    a->dummy_sk = opt_arr(r, 32);
    proprietary(r);
    /* Output */
    a->cmx = v1 ? arr(r, 32) : opt_arr(r, 32);
    a->epk = arr(r, 32);
    if (v1) {
        a->enc_kind = ZC_ENC_CIPHERTEXT;
        a->enc = vec_u8(r);
    } else {
        uint32_t variant = vu32(r);
        if (OK(r) && variant > 1) SETERR(r, ZC_R_PARSE);
        a->enc_kind = (uint8_t)variant;
        a->enc = vec_u8(r);
        /* MemoPlaintext: at most 512 bytes, trailing zeros stripped. */
        if (OK(r) && variant == ZC_ENC_MEMO &&
            (a->enc.len > 512 || (a->enc.len && a->enc.p[a->enc.len - 1] == 0)))
            SETERR(r, ZC_R_PARSE);
    }
    a->out_ct = vec_u8(r);
    a->o_recipient = opt_arr(r, 43);
    a->has_o_value = (uint8_t)opt(r);
    if (a->has_o_value) a->o_value = vu64(r);
    a->o_rseed = opt_arr(r, 32);
    a->ock = opt_arr(r, 32);
    a->o_zip32_present = (uint8_t)opt(r);
    if (a->o_zip32_present) a->o_zip32_hardened = zip32(r);
    a->user_address = opt_string(r);
    proprietary(r);
    /* Action.rcv */
    a->rcv = opt_arr(r, 32);
}

static void o_bundle(rd *r, zc_pczt *z, zc_obundle *b, int v1)
{
    uint32_t n = count(r);
    b->first = z->n_actions;
    for (uint32_t i = 0; i < n && OK(r); i++) {
        static zc_action scratch;
        if (z->n_actions < ZC_MAX_ACTIONS) {
            o_action(r, &z->actions[z->n_actions++], v1);
            b->n++;
        } else {
            r->over_cap = 1;
            o_action(r, &scratch, v1);
        }
    }
    b->flags = u8(r);
    b->value_sum_mag = vu64(r);
    b->value_sum_neg = boolean(r);
    if (v1) {
        const uint8_t *anchor = arr(r, 32);
        static const uint8_t zero[32];
        /* v1 has no "absent": an empty bundle's all-zero anchor reads as None. */
        b->anchor = (anchor && n == 0 && memcmp(anchor, zero, 32) == 0) ? NULL : anchor;
        b->note_version = 2;
    } else {
        b->anchor = opt_arr(r, 32);
        uint32_t nv = vu32(r);                /* SerializedNoteVersion */
        if (OK(r) && nv > 1) SETERR(r, ZC_R_PARSE);
        b->note_version = (uint8_t)(nv + 2);
    }
    b->has_zkproof = (uint8_t)opt(r);
    if (b->has_zkproof) vec_u8(r);
    b->bsk = opt_arr(r, 32);
}

static void empty_bundle(zc_pczt *z, zc_obundle *b, uint8_t flags, uint8_t nv)
{
    memset(b, 0, sizeof *b);
    b->first = z->n_actions;
    b->flags = flags;
    b->note_version = nv;
}

int zc_pczt_read(const uint8_t *buf, size_t len, zc_pczt *z)
{
    if (!buf || !z) return ZC_R_PARSE;
    memset(z, 0, sizeof *z);
    if (len < 8 || memcmp(buf, "PCZT", 4) != 0) return ZC_R_PARSE;
    z->version = (uint32_t)buf[4] | (uint32_t)buf[5] << 8 |
                 (uint32_t)buf[6] << 16 | (uint32_t)buf[7] << 24;
    if (z->version != 1 && z->version != 2) return ZC_R_PARSE;

    rd r = { buf + 8, len - 8, 0, 0 };
    global(&r, &z->g);
    if (z->version == 1) {
        transparent(&r, z);
        sapling(&r, &z->sapling, 1);
        o_bundle(&r, z, &z->orchard, 1);
        empty_bundle(z, &z->ironwood, 0x07, 3);
    } else {
        if (opt(&r)) transparent(&r, z);
        if (opt(&r)) sapling(&r, &z->sapling, 0);
        if (opt(&r)) o_bundle(&r, z, &z->orchard, 0);
        else empty_bundle(z, &z->orchard, 0x03, 2);
        if (opt(&r)) o_bundle(&r, z, &z->ironwood, 0);
        else empty_bundle(z, &z->ironwood, 0x07, 3);
    }
    if (r.err) return r.err;
    if (r.n != 0) {                     /* trailing bytes */
        SETERR(&r, ZC_R_PARSE);
        return ZC_R_PARSE;
    }
    return r.over_cap ? ZC_R_TOO_MANY : ZC_R_OK;
}
