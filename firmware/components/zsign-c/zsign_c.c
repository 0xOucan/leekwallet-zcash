/* LeekWallet Zcash: the C signer backend (see include/zsign_c.h).
 *
 * It mirrors components/zsign-rust/src/signer.rs, which runs librustzcash's
 * pczt Verifier and Signer roles. Where those roles reject a PCZT, this code
 * rejects it too, in the same order and with the same ZS_E_* code wherever
 * practical, so a disagreement between the two backends means a bug in one of
 * them and not a difference in policy. The stages:
 *
 *   1. zc_pczt_new   wire format (pczt_reader.c)               -> ZS_E_PARSE
 *   2. check()       the Rust check(): branch, network, our keys,
 *                    unsupported bundles, transparent outputs, then per
 *                    Orchard-protocol pool: the field validation that
 *                    orchard::pczt parsing performs (validate_pool) and the
 *                    signer checks (check_pool), then the fee
 *   3. prepare()     what pczt::Signer::new adds: tx version, value ranges,
 *                    rk / epk validity, and the v6 sighash, built from the
 *                    parsed fields (ZIP 244 as amended by ZIP 229)
 *
 * Every value the summary shows is recomputed here: cmx binds each output's
 * recipient, value and rseed; cv_net binds values to the commitments the
 * proof uses; sum(spend - output) must equal value_sum; spends we sign must
 * match our FVK through the nullifier (Poseidon) and rk.
 *
 * Memory: no heap. Large scratch buffers are static (the signer is
 * single-threaded); every one that held a secret is wiped before return.
 */
#include "zsign_c.h"

#include <string.h>

#include "blake2b.h"
#include "memzero.h"
#include "pczt_reader.h"
#include "zc_crypto.h"
#include "zc_encoding.h"
#include "zip244.h"

#define BRANCH_NU6_3 0x37a5165bu
#define BRANCH_NU7   0x77190ad9u
#define V6_TX_VERSION 6u
#define V6_VERSION_GROUP_ID 0xD884B698u
#define MAX_MONEY 2100000000000000ull

#define ZS_POOL(p) ((p) == 0 ? ZS_POOL_ORCHARD : ZS_POOL_IRONWOOD)

struct zc_ctx {
    zc_pczt z;
    const uint8_t *bytes;
    size_t len;
    uint8_t digest[32];          /* BLAKE2b-256 of bytes at zc_pczt_new */
    /* Resolved fields (v2 may omit cv_net and cmx; they are recomputed). */
    uint8_t cv[ZC_MAX_ACTIONS][32];
    uint8_t cmx[ZC_MAX_ACTIONS][32];
    uint8_t valid[2];            /* validate_pool passed, per pool */
    int32_t prepared;            /* 0 not yet, 1 ok, else the refusal */
    uint8_t sighash[32];
    uint8_t checked;             /* a check passed and no signing used it */
    uint8_t checked_net;
    uint32_t checked_account;
    uint8_t checked_fvk[96];
    uint8_t is_static;
};

static zc_ctx s_ctx;
static uint8_t s_ctx_busy;

/* ---- helpers ------------------------------------------------------------ */

static void digest_bytes(const uint8_t *b, size_t n, uint8_t out[32])
{
    blake2b_state s;
    blake2b_InitPersonal(&s, 32, "LeekZ_PCZT_bytes", 16);
    blake2b_Update(&s, b, n);
    blake2b_Final(&s, out, 32);
}

static int ct_eq(const uint8_t *a, const uint8_t *b, size_t n)
{
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) d |= a[i] ^ b[i];
    return d == 0;
}

/* Address::from_raw_address_bytes: pk_d a non-identity point (d is free). */
static int valid_recipient(const uint8_t r[43]) { return zc_point_valid_nonid(r + 11); }

static uint8_t note_version_of(int pool) { return pool == 0 ? 2 : 3; }

static const zc_obundle *bundle_of(const zc_pczt *z, int pool)
{
    return pool == 0 ? &z->orchard : &z->ironwood;
}

/* The resolved 580-byte enc_ciphertext of an action: as carried, or
 * re-encrypted from the memo plaintext (v2 compact form), whose epk must
 * equal the carried one. 0 on success. */
static int enc_ciphertext(int pool, const zc_action *a, uint8_t out[580])
{
    if (a->enc_kind == ZC_ENC_CIPHERTEXT) {
        if (a->enc.len != 580) return -1;
        memcpy(out, a->enc.p, 580);
        return 0;
    }
    static uint8_t memo[512];
    uint8_t epk[32];
    if (!a->o_recipient || !a->has_o_value || !a->o_rseed) return -1;
    memset(memo, 0, sizeof memo);
    memcpy(memo, a->enc.p, a->enc.len);
    int rc = zc_note_encrypt(out, epk, a->o_recipient, a->o_value, a->o_rseed, a->nullifier,
                             memo, note_version_of(pool));
    memzero(memo, sizeof memo);
    if (rc != 0) return -1;
    return memcmp(epk, a->epk, 32) == 0 ? 0 : -1;
}

/* ---- stage 2a: field validation (orchard::pczt parsing) ----------------- */

/* pczt Bundle::into_parsed_with_version: resolve_fields, the anchor rule,
 * then Spend/Output/Action/Bundle::parse. Any failure is ZS_E_PARSE, as in
 * the Rust check (a Verifier parse error).
 *
 * One deliberate shortcut: for an `fvk` field, the Rust parse also derives
 * both incoming viewing keys and rejects the key if either is zero. That
 * costs two Sinsemilla commitments per key, and a structurally valid key
 * fails it with probability ~2^-250, so only the structure is checked here.
 * A key that is actually used (check_pool) is fully derived there. The same
 * holds for `dummy_sk`, which the check refuses outright either way. */
static int fvk_structurally_valid(const uint8_t f[96])
{
    static const uint8_t zero[32];
    if (memcmp(f, zero, 32) == 0 || (f[31] & 0x80)) return 0;
    if (!zc_point_valid(f)) return 0;
    return zc_fp_canonical(f + 32) && zc_fq_canonical(f + 64);
}

static int32_t validate_pool(zc_ctx *c, int pool)
{
    if (c->valid[pool]) return 0;
    zc_pczt *z = &c->z;
    const zc_obundle *b = bundle_of(z, pool);
    uint8_t nv = b->note_version;       /* resolve_fields uses the wire version */
    static uint8_t enc[580];

    /* resolve_fields: cv_net, cmx, enc_ciphertext, in action order. */
    for (int i = 0; i < b->n; i++) {
        int k = b->first + i;
        const zc_action *a = &z->actions[k];
        if (a->cv_net) {
            memcpy(c->cv[k], a->cv_net, 32);
        } else {
            if (!a->has_s_value || !a->has_o_value || !a->rcv || !zc_fq_canonical(a->rcv))
                return ZS_E_PARSE;
            zc_value_commit(c->cv[k], a->s_value, a->o_value, a->rcv);
        }
        if (a->cmx) {
            memcpy(c->cmx[k], a->cmx, 32);
        } else {
            if (!a->o_recipient || !valid_recipient(a->o_recipient)) return ZS_E_PARSE;
            if (!zc_fp_canonical(a->nullifier)) return ZS_E_PARSE;
            if (!a->o_rseed || !zc_rseed_valid(a->o_rseed, a->nullifier)) return ZS_E_PARSE;
            if (!a->has_o_value) return ZS_E_PARSE;
            if (zc_note_cmx(c->cmx[k], a->o_recipient, a->o_value, a->nullifier, a->o_rseed,
                            nv) != 0)
                return ZS_E_PARSE;
        }
        if (a->enc_kind == ZC_ENC_MEMO) {
            if (!a->o_recipient || !valid_recipient(a->o_recipient)) return ZS_E_PARSE;
            if (!zc_fp_canonical(a->nullifier)) return ZS_E_PARSE;
            if (!a->o_rseed || !zc_rseed_valid(a->o_rseed, a->nullifier)) return ZS_E_PARSE;
            if (!a->has_o_value) return ZS_E_PARSE;
            if (enc_ciphertext(pool, a, enc) != 0) return ZS_E_PARSE;
        }
    }
    /* AnchorRequirement::for_pre_authorization: only a v6 transaction may
     * leave a non-empty bundle's anchor absent (ZIP 374). */
    if (!b->anchor && b->n && z->g.tx_version != V6_TX_VERSION) return ZS_E_PARSE;

    for (int i = 0; i < b->n; i++) {
        int k = b->first + i;
        const zc_action *a = &z->actions[k];
        /* Spend::parse */
        if (a->s_zip32_present && !a->s_zip32_hardened) return ZS_E_PARSE;
        if (!zc_fp_canonical(a->nullifier)) return ZS_E_PARSE;
        if (!zc_point_valid(a->rk)) return ZS_E_PARSE;
        if (a->s_recipient && !valid_recipient(a->s_recipient)) return ZS_E_PARSE;
        if (a->rho && !zc_fp_canonical(a->rho)) return ZS_E_PARSE;
        if (a->s_rseed && (!a->rho || !zc_rseed_valid(a->s_rseed, a->rho))) return ZS_E_PARSE;
        if (a->fvk && !fvk_structurally_valid(a->fvk)) return ZS_E_PARSE;
        if (a->witness)
            for (int j = 0; j < 32; j++)
                if (!zc_fp_canonical(a->witness + 32 * j)) return ZS_E_PARSE;
        if (a->alpha && !zc_fq_canonical(a->alpha)) return ZS_E_PARSE;
        /* Output::parse */
        if (a->enc_kind == ZC_ENC_CIPHERTEXT && a->enc.len != 580) return ZS_E_PARSE;
        if (a->out_ct.len != 80) return ZS_E_PARSE;
        if (!zc_fp_canonical(c->cmx[k])) return ZS_E_PARSE;
        if (a->o_recipient && !valid_recipient(a->o_recipient)) return ZS_E_PARSE;
        if (a->o_rseed && !zc_rseed_valid(a->o_rseed, a->nullifier)) return ZS_E_PARSE;
        if (a->o_zip32_present && !a->o_zip32_hardened) return ZS_E_PARSE;
        /* Action::parse */
        if (!zc_point_valid(c->cv[k])) return ZS_E_PARSE;
        if (a->rcv && !zc_fq_canonical(a->rcv)) return ZS_E_PARSE;
    }
    /* Bundle::parse: flags (bits 3..7 clear; bit 2 only for Ironwood), the
     * pool's note version, anchor, bsk. */
    if (b->flags & 0xF8) return ZS_E_PARSE;
    if (pool == 0 && (b->flags & 0x04)) return ZS_E_PARSE;
    if (b->n && b->note_version != note_version_of(pool)) return ZS_E_PARSE;
    if (b->anchor && !zc_fp_canonical(b->anchor)) return ZS_E_PARSE;
    if (b->bsk && !zc_fq_canonical(b->bsk)) return ZS_E_PARSE;
    c->valid[pool] = 1;
    return 0;
}

/* ---- stage 2b: the signer's checks (signer.rs check_bundle) ------------- */

typedef struct {
    const zc_fvk *fvk;
    const uint8_t *fvk96;
    uint8_t net;
    zs_summary *out;
    uint8_t n_out;               /* outputs collected (saturating) */
    uint8_t n_spends, n_dummies;
    uint32_t warnings;
    uint64_t total_in;
    uint64_t shielded_out;
    int over;                    /* more outputs than ZS_MAX_OUTPUTS */
} acc_t;

static void put_str(char *dst, size_t cap, const char *s, size_t n)
{
    if (n > cap - 1) n = cap - 1;
    memcpy(dst, s, n);
    memset(dst + n, 0, cap - n);
}

static zs_output *next_output(acc_t *acc)
{
    static zs_output scratch;
    if (acc->n_out >= ZS_MAX_OUTPUTS) {
        acc->over = 1;
        if (acc->n_out < 255) acc->n_out++;
        memset(&scratch, 0, sizeof scratch);
        return &scratch;
    }
    zs_output *o = &acc->out->outputs[acc->n_out++];
    memset(o, 0, sizeof *o);
    return o;
}

/* ZIP 302, as signer.rs classify_memo. */
static void classify_memo(zs_output *o, const uint8_t m[512])
{
    int rest_zero = 1;
    for (int i = 1; i < 512; i++) if (m[i]) { rest_zero = 0; break; }
    if (m[0] == 0xF6 && rest_zero) {
        o->memo_kind = ZS_MEMO_NONE;
        o->memo_len = 0;
        return;
    }
    if (m[0] <= 0xF4) {
        int end = 512;
        while (end > 0 && m[end - 1] == 0) end--;
        if (zc_utf8_valid(m, (size_t)end)) {
            o->memo_kind = ZS_MEMO_TEXT;
            o->memo_len = (uint16_t)end;
            memcpy(o->memo, m, (size_t)end);
            return;
        }
    }
    o->memo_kind = ZS_MEMO_BINARY;
    o->memo_len = 512;
    memcpy(o->memo, m, 512);
}

/* Spend::verify_nullifier with a given FVK: the spent note must be complete,
 * belong to that FVK (either scope), and hash to the action's nullifier. */
static int verify_nullifier(const zc_action *a, const zc_fvk *f, uint8_t nv)
{
    zc_pt cm;
    uint8_t psi[32], nf[32];
    if (!a->s_recipient || !a->has_s_value || !a->rho || !a->s_rseed) return -1;
    if (zc_note_commit(&cm, a->s_recipient, a->s_value, a->rho, a->s_rseed, nv) != 0) return -1;
    if (zc_fvk_scope(f, a->s_recipient) == 0) return -1;
    zc_note_psi(psi, a->s_rseed, a->rho);
    zc_nullifier(nf, f->nk, a->rho, psi, &cm);
    memzero(psi, sizeof psi);
    return memcmp(nf, a->nullifier, 32) == 0 ? 0 : -1;
}

/* Spend::verify_rk: rk == ak + [alpha] G. */
static int verify_rk(const zc_action *a, const uint8_t ak[32])
{
    uint8_t rk[32];
    if (!a->alpha) return -1;
    if (zc_randomize_ak(rk, ak, a->alpha) != 0) return -1;
    return memcmp(rk, a->rk, 32) == 0 ? 0 : -1;
}

static int32_t check_pool(zc_ctx *c, int pool, acc_t *acc)
{
    zc_pczt *z = &c->z;
    const zc_obundle *b = bundle_of(z, pool);
    uint8_t nv = note_version_of(pool);
    static zc_fvk other;
    static uint8_t enc[580], memo[512], rec[43];
    int32_t rc = 0;

    /* verify_cross_address_restriction: always on for Orchard under protocol
     * V3, and for Ironwood unless flag bit 2 enables cross-address actions. */
    int cross = pool == 1 && (b->flags & 0x04);
    if (!cross) {
        for (int i = 0; i < b->n; i++) {
            const zc_action *a = &z->actions[b->first + i];
            if (!a->s_recipient || !a->o_recipient) return ZS_E_OUTPUT_COMMITMENT;
            if (!zc_same_receiver(a->s_recipient, a->o_recipient)) return ZS_E_OUTPUT_COMMITMENT;
        }
    }

    /* sum(spend - output) as a 128-bit difference: in and out < 2^70 each. */
    uint64_t in_lo = 0, in_hi = 0, out_lo = 0, out_hi = 0;

    for (int i = 0; i < b->n; i++) {
        int k = b->first + i;
        const zc_action *a = &z->actions[k];
        if (a->dummy_sk) return ZS_E_DUMMY_KEY;
        if (!a->has_s_value) return ZS_E_MISSING_DATA;
        uint64_t sv = a->s_value;
        if (!a->spend_auth_sig) {
            /* Ours to sign, so it must be provably ours. As orchard's
             * fvk_for_validation: a spend that names an FVK is checked against
             * that FVK, which must be ours unless the note is zero-valued. */
            const zc_fvk *f = acc->fvk;
            if (a->fvk && memcmp(a->fvk, acc->fvk96, 96) != 0) {
                if (sv != 0) return ZS_E_SPEND_NOT_OURS;
                if (zc_fvk_from_bytes(&other, a->fvk) != 0) return ZS_E_PARSE;
                f = &other;
            }
            int bad = verify_nullifier(a, f, nv) != 0 || verify_rk(a, f->ak) != 0;
            memzero(&other, sizeof other);
            if (bad) return ZS_E_SPEND_NOT_OURS;
            if (sv > 0) {
                acc->n_spends++;
                acc->total_in += sv;
            }
        } else if (sv != 0) {
            /* A pre-signed spend is only acceptable as zero-value padding. */
            return ZS_E_SPEND_NOT_OURS;
        }

        /* verify_note_commitment: rho is this action's nullifier. */
        if (!a->o_recipient || !a->has_o_value || !a->o_rseed) return ZS_E_OUTPUT_COMMITMENT;
        {
            uint8_t cmx[32];
            if (zc_note_cmx(cmx, a->o_recipient, a->o_value, a->nullifier, a->o_rseed, nv) != 0 ||
                memcmp(cmx, c->cmx[k], 32) != 0)
                return ZS_E_OUTPUT_COMMITMENT;
        }
        if (a->rcv) {
            uint8_t cv[32];
            zc_value_commit(cv, sv, a->o_value, a->rcv);
            if (memcmp(cv, c->cv[k], 32) != 0) return ZS_E_VALUE_BALANCE;
        }
        uint64_t ov = a->o_value;
        in_lo += sv;
        if (in_lo < sv) in_hi++;
        out_lo += ov;
        if (out_lo < ov) out_hi++;

        int scope = zc_fvk_scope(acc->fvk, a->o_recipient);
        if (ov == 0 && !a->user_address.p) {
            acc->n_dummies++;
            continue;
        }
        zs_output *o = next_output(acc);
        o->pool = (uint8_t)ZS_POOL(pool);
        o->value = ov;
        if (scope == 2) {
            o->kind = ZS_KIND_CHANGE;
        } else if (a->user_address.p) {
            if (!zc_ua_contains_orchard((const char *)a->user_address.p, a->user_address.len,
                                        acc->net, a->o_recipient))
                return ZS_E_USER_ADDRESS;
            o->kind = scope ? ZS_KIND_SELF : ZS_KIND_EXTERNAL;
            put_str(o->address, sizeof o->address, (const char *)a->user_address.p,
                    a->user_address.len);
        } else {
            if (!scope) acc->warnings |= ZS_WARN_NO_USER_ADDRESS;
            o->kind = scope ? ZS_KIND_SELF : ZS_KIND_EXTERNAL;
            if (!zc_encode_ua(o->address, sizeof o->address, acc->net, a->o_recipient, NULL))
                return ZS_E_INTERNAL;
        }

        /* Memo: recovered with this account's own OVKs (external, then
         * internal), as signer.rs does; unrecovered otherwise. */
        o->memo_kind = ZS_MEMO_UNRECOVERED;
        if (enc_ciphertext(pool, a, enc) == 0) {
            const uint8_t *ovks[2] = { acc->fvk->ovk_ext, acc->fvk->ovk_int };
            for (int j = 0; j < 2; j++) {
                uint64_t v;
                if (zc_recover_with_ovk(ovks[j], c->cv[k], c->cmx[k], a->epk, enc,
                                        a->out_ct.p, a->nullifier, nv, rec, &v, memo) == 0 &&
                    memcmp(rec, a->o_recipient, 43) == 0 && v == ov) {
                    classify_memo(o, memo);
                    break;
                }
            }
        }
        memzero(memo, sizeof memo);
        acc->shielded_out += ov;
    }

    /* value_sum must fit an i64 and equal sum(spend - output). */
    uint64_t mag = b->value_sum_mag;
    if (!b->value_sum_neg ? mag > 0x7FFFFFFFFFFFFFFFull : mag > 0x8000000000000000ull)
        return ZS_E_VALUE_BALANCE;
    if (!b->value_sum_neg || mag == 0) {
        /* in - out == mag  <=>  in == out + mag */
        uint64_t lo = out_lo + mag, hi = out_hi + (out_lo + mag < out_lo);
        if (lo != in_lo || hi != in_hi) rc = ZS_E_VALUE_BALANCE;
    } else {
        uint64_t lo = in_lo + mag, hi = in_hi + (in_lo + mag < in_lo);
        if (lo != out_lo || hi != out_hi) rc = ZS_E_VALUE_BALANCE;
    }
    return rc;
}

/* ---- stage 3: what pczt::Signer::new adds, and the sighash -------------- */

static void hash_t_outputs(const zc_pczt *z, uint8_t digest[32])
{
    if (z->n_tout == 0 && z->n_tin == 0) {
        zip244_empty_digest("ZTxIdTranspaHash", digest);
        return;
    }
    /* No inputs (refused earlier), so prevouts and sequences are empty. */
    blake2b_state prev, seq, outs, root;
    uint8_t dp[32], ds[32], dout[32];
    blake2b_InitPersonal(&prev, 32, "ZTxIdPrevoutHash", 16);
    blake2b_InitPersonal(&seq, 32, "ZTxIdSequencHash", 16);
    blake2b_InitPersonal(&outs, 32, "ZTxIdOutputsHash", 16);
    for (int i = 0; i < z->n_tout; i++) {
        const zc_tout *o = &z->tout[i];
        uint8_t v[8], cs[5];
        size_t n = o->script_pubkey.len, csl;
        for (int j = 0; j < 8; j++) v[j] = (uint8_t)(o->value >> (8 * j));
        if (n < 253) { cs[0] = (uint8_t)n; csl = 1; }
        else if (n <= 0xFFFF) { cs[0] = 0xFD; cs[1] = (uint8_t)n; cs[2] = (uint8_t)(n >> 8); csl = 3; }
        else { cs[0] = 0xFE; for (int j = 0; j < 4; j++) cs[1 + j] = (uint8_t)(n >> (8 * j)); csl = 5; }
        blake2b_Update(&outs, v, 8);
        blake2b_Update(&outs, cs, csl);
        blake2b_Update(&outs, o->script_pubkey.p, n);
    }
    blake2b_Final(&prev, dp, 32);
    blake2b_Final(&seq, ds, 32);
    blake2b_Final(&outs, dout, 32);
    blake2b_InitPersonal(&root, 32, "ZTxIdTranspaHash", 16);
    blake2b_Update(&root, dp, 32);
    blake2b_Update(&root, ds, 32);
    blake2b_Update(&root, dout, 32);
    blake2b_Final(&root, digest, 32);
}

/* A value sum as a ZatBalance: |v| <= MAX_MONEY. */
static int value_balance(const zc_obundle *b, int64_t *v)
{
    if (b->value_sum_mag > MAX_MONEY) return -1;
    *v = b->value_sum_neg ? -(int64_t)b->value_sum_mag : (int64_t)b->value_sum_mag;
    return 0;
}

static int32_t prepare(zc_ctx *c)
{
    if (c->prepared) return c->prepared == 1 ? 0 : c->prepared;
    zc_pczt *z = &c->z;
    int32_t rc = ZS_E_PARSE;
    static Zip244ActionsState st[2];
    static Zip244TxMeta meta;
    static uint8_t action[820];

    /* extract_tx_data: a known branch with Orchard protocol V3, and the v6
     * format (the v5 format, which librustzcash still extracts for an
     * Orchard-only bundle, is refused by both backends). */
    if (z->g.consensus_branch_id != BRANCH_NU6_3 && z->g.consensus_branch_id != BRANCH_NU7)
        goto out;
    if (z->g.tx_version != V6_TX_VERSION || z->g.version_group_id != V6_VERSION_GROUP_ID)
        goto out;
    /* transparent into_parsed: Output::parse */
    if (z->n_tin) goto out;
    for (int i = 0; i < z->n_tout; i++) {
        const zc_tout *o = &z->tout[i];
        if (o->value > MAX_MONEY) goto out;
        if (!zc_script_parses(o->script_pubkey.p, o->script_pubkey.len)) goto out;
        if (o->redeem_script.p && !zc_script_parses(o->redeem_script.p, o->redeem_script.len))
            goto out;
    }
    /* sapling into_parsed + extract_effects on an empty bundle. */
    if (z->sapling.present) {
        if (z->sapling.n_spends || z->sapling.n_outputs) goto out;
        if (z->sapling.anchor && !zc_bls_fr_canonical(z->sapling.anchor)) goto out;
        if (z->sapling.bsk && !zc_jubjub_fr_canonical(z->sapling.bsk)) goto out;
        if (!z->sapling.value_sum_fits_i64) goto out;
        int64_t sv = z->sapling.value_sum;
        if (sv > (int64_t)MAX_MONEY || sv < -(int64_t)MAX_MONEY) goto out;
    }
    for (int p = 0; p < 2; p++)
        if ((rc = validate_pool(c, p)) != 0) goto out;
    rc = ZS_E_PARSE;

    memset(&meta, 0, sizeof meta);
    meta.version = ZIP244_V6_TX_VERSION;
    meta.version_group_id = z->g.version_group_id;
    meta.consensus_branch_id = z->g.consensus_branch_id;
    meta.lock_time = z->g.has_fallback_lock_time ? z->g.fallback_lock_time : 0;
    meta.expiry_height = z->g.expiry_height;
    meta.is_v6 = true;
    hash_t_outputs(z, meta.transparent_sig_digest);
    zip244_sapling_empty_digest(meta.sapling_digest);

    for (int p = 0; p < 2; p++) {
        const zc_obundle *b = bundle_of(z, p);
        zip244_actions_init_pool(&st[p], p == 0 ? ZIP244_POOL_ORCHARD : ZIP244_POOL_IRONWOOD, true);
        if (!b->n) continue;
        int64_t vb;
        if (value_balance(b, &vb) != 0) goto out;      /* ValueSumOutOfRange */
        if (p == 0) { meta.orchard_flags = b->flags; meta.value_balance = vb; }
        else { meta.ironwood_flags = b->flags; meta.ironwood_value_balance = vb; }
        for (int i = 0; i < b->n; i++) {
            int k = b->first + i;
            const zc_action *a = &z->actions[k];
            /* orchard::Action::from_parts: rk not the identity, epk a valid
             * non-identity point. */
            if (!zc_point_valid_nonid(a->rk)) goto out;
            if (!zc_point_valid_nonid(a->epk)) goto out;
            memcpy(action + 0, c->cv[k], 32);
            memcpy(action + 32, a->nullifier, 32);
            memcpy(action + 64, a->rk, 32);
            memcpy(action + 96, c->cmx[k], 32);
            memcpy(action + 128, a->epk, 32);
            if (enc_ciphertext(p, a, action + 160) != 0) goto out;
            memcpy(action + 740, a->out_ct.p, 80);
            if (!zip244_hash_action(&st[p], action, sizeof action)) goto out;
        }
    }
    zip244_shielded_sighash_v6(&meta, &st[0], &st[1], c->sighash);
    rc = 0;
out:
    c->prepared = rc == 0 ? 1 : rc;
    return rc;
}

/* ---- the check ---------------------------------------------------------- */

static int32_t check(zc_ctx *c, const uint8_t seed[64], uint32_t account, uint8_t net,
                     zs_summary *out, uint8_t fvk_out[96])
{
    zc_pczt *z = &c->z;
    static zc_fvk fvk;
    uint8_t ask[32];
    acc_t acc;
    int32_t rc;
    uint32_t branch = z->g.consensus_branch_id;

    if (branch != BRANCH_NU6_3 && branch != BRANCH_NU7) return ZS_E_UNSUPPORTED_BRANCH;
    if (z->g.coin_type != (net == ZS_NET_MAIN ? 133u : 1u)) return ZS_E_NETWORK;
    if (zc_account_keys(seed, net == ZS_NET_MAIN ? 133 : 1, account, ask, &fvk) != 0)
        return ZS_E_INTERNAL;
    memzero(ask, sizeof ask);
    memcpy(fvk_out, fvk.ak, 32);
    memcpy(fvk_out + 32, fvk.nk, 32);
    memcpy(fvk_out + 64, fvk.rivk, 32);

    /* Not yet supported on the device: refuse rather than sign blind. */
    if (z->n_tin) { rc = ZS_E_UNSUPPORTED_BUNDLE; goto done; }
    if (z->sapling.n_spends || z->sapling.n_outputs) { rc = ZS_E_UNSUPPORTED_BUNDLE; goto done; }

    memset(&acc, 0, sizeof acc);
    acc.fvk = &fvk;
    acc.fvk96 = fvk_out;
    acc.net = net;
    acc.out = out;

    uint64_t t_out = 0;
    for (int i = 0; i < z->n_tout; i++) {
        const zc_tout *t = &z->tout[i];
        char addr[64];
        if (!zc_t_address(addr, sizeof addr, net, t->script_pubkey.p, t->script_pubkey.len)) {
            rc = ZS_E_UNSUPPORTED_BUNDLE;
            goto done;
        }
        if (t_out + t->value < t_out) { rc = ZS_E_VALUE_BALANCE; goto done; }
        t_out += t->value;
        acc.warnings |= ZS_WARN_TRANSPARENT_OUT;
        zs_output *o = next_output(&acc);
        o->pool = ZS_POOL_TRANSPARENT;
        o->kind = ZS_KIND_EXTERNAL;
        o->value = t->value;
        put_str(o->address, sizeof o->address, addr, strlen(addr));
        o->memo_kind = ZS_MEMO_NONE;
    }

    for (int p = 0; p < 2; p++) {
        if ((rc = validate_pool(c, p)) != 0) goto done;
        if ((rc = check_pool(c, p, &acc)) != 0) goto done;
    }

    /* Fee = everything in - everything out, computed here from bound values. */
    if (acc.shielded_out > acc.total_in || t_out > acc.total_in - acc.shielded_out) {
        rc = ZS_E_VALUE_BALANCE;
        goto done;
    }
    uint64_t fee = acc.total_in - acc.shielded_out - t_out;
    if (acc.over) { rc = ZS_E_TOO_MANY; goto done; }
    if ((rc = prepare(c)) != 0) goto done;

    out->branch_id = branch;
    out->tx_version = z->g.tx_version;
    out->warnings = acc.warnings;
    out->n_outputs = acc.n_out;
    out->n_spends = acc.n_spends;
    out->n_dummies = acc.n_dummies;
    out->total_in = acc.total_in;
    out->fee = fee;
    memcpy(out->sighash, c->sighash, 32);
    rc = 0;
done:
    memzero(&fvk, sizeof fvk);
    return rc;
}

/* ---- public API ---------------------------------------------------------- */

size_t zc_ctx_size(void) { return sizeof(zc_ctx); }

zc_ctx *zc_pczt_new_in(void *mem, size_t mem_len, const uint8_t *bytes, size_t len,
                       int32_t *err)
{
    zc_ctx *c = (zc_ctx *)mem;
#define SET(e) do { if (err) *err = (e); } while (0)
    if (!mem || mem_len < sizeof(zc_ctx) || ((uintptr_t)mem % sizeof(void *))) {
        SET(ZS_ERR_ARG);
        return NULL;
    }
    if (!bytes) { SET(ZS_E_PARSE); return NULL; }
    if (zc_crypto_init() != 0) { SET(ZS_E_INTERNAL); return NULL; }
    memset(c, 0, sizeof *c);
    int r = zc_pczt_read(bytes, len, &c->z);
    if (r != ZC_R_OK) {
        memset(c, 0, sizeof *c);
        SET(r == ZC_R_TOO_MANY ? ZS_E_TOO_MANY : ZS_E_PARSE);
        return NULL;
    }
    c->bytes = bytes;
    c->len = len;
    digest_bytes(bytes, len, c->digest);
    SET(0);
    return c;
#undef SET
}

zc_ctx *zc_pczt_new(const uint8_t *bytes, size_t len, int32_t *err)
{
    if (s_ctx_busy) {
        if (err) *err = ZS_E_INTERNAL;
        return NULL;
    }
    zc_ctx *c = zc_pczt_new_in(&s_ctx, sizeof s_ctx, bytes, len, err);
    if (c) {
        c->is_static = 1;
        s_ctx_busy = 1;
    }
    return c;
}

void zc_pczt_free(zc_ctx *ctx)
{
    if (!ctx) return;
    int st = ctx->is_static;
    memzero(ctx, sizeof *ctx);
    if (st) s_ctx_busy = 0;
}

int32_t zc_pczt_check(zc_ctx *ctx, const uint8_t seed[64], uint32_t account, uint8_t net,
                      zs_summary *out)
{
    if (!ctx || !seed || !out) return ZS_ERR_ARG;
    if (net != ZS_NET_MAIN && net != ZS_NET_TEST) return ZS_ERR_ARG;
    ctx->checked = 0;
    memset(out, 0, sizeof *out);
    uint8_t fvk[96];
    int32_t rc = check(ctx, seed, account, net, out, fvk);
    if (rc == 0) {
        ctx->checked = 1;
        ctx->checked_net = net;
        ctx->checked_account = account;
        memcpy(ctx->checked_fvk, fvk, 96);
    } else {
        memset(out, 0, sizeof *out);     /* as the Rust side: nothing on refusal */
    }
    memzero(fvk, sizeof fvk);
    return rc;
}

int32_t zc_pczt_sign(zc_ctx *ctx, const uint8_t seed[64], uint32_t account, uint8_t net,
                     zs_sig *sigs, uint32_t max, uint32_t *n_out)
{
    static zc_fvk fvk;
    uint8_t ask[32], digest[32], rk[32], mine[96];
    if (!ctx || !seed || !sigs || !n_out) return ZS_ERR_ARG;
    if (net != ZS_NET_MAIN && net != ZS_NET_TEST) return ZS_ERR_ARG;
    if (!ctx->checked) return ZS_E_NOT_CHECKED;
    ctx->checked = 0;                    /* one check authorises one signing */
    /* The bytes we checked must be the bytes we sign. */
    digest_bytes(ctx->bytes, ctx->len, digest);
    if (!ct_eq(digest, ctx->digest, 32)) return ZS_E_NOT_CHECKED;
    if (zc_account_keys(seed, net == ZS_NET_MAIN ? 133 : 1, account, ask, &fvk) != 0)
        return ZS_E_INTERNAL;
    int32_t rc = 0;
    memcpy(mine, fvk.ak, 32);
    memcpy(mine + 32, fvk.nk, 32);
    memcpy(mine + 64, fvk.rivk, 32);
    if (net != ctx->checked_net || !ct_eq(mine, ctx->checked_fvk, 96)) {
        rc = ZS_E_NOT_CHECKED;
        goto done;
    }
    if ((rc = prepare(ctx)) != 0) goto done;

    /* Every unsigned spend, Orchard pool first, as zs_pczt_sign. */
    uint32_t n = 0;
    static zs_sig scratch;
    for (int p = 0; p < 2; p++) {
        const zc_obundle *b = bundle_of(&ctx->z, p);
        for (int i = 0; i < b->n; i++) {
            const zc_action *a = &ctx->z.actions[b->first + i];
            if (a->spend_auth_sig) continue;
            /* pczt Signer: verify_nullifier(None) needs the spend's own FVK
             * (the check bound it to ours), the Signer needs alpha, and rk
             * must be [ask + alpha] G. */
            if (!a->fvk || !a->alpha) { rc = ZS_E_SIGN; goto done; }
            zs_sig *s = n < max ? &sigs[n] : &scratch;
            memset(s, 0, sizeof *s);
            if (zc_redpallas_sign(ask, a->alpha, ctx->sighash, s->sig, rk) != 0 ||
                memcmp(rk, a->rk, 32) != 0) {
                memset(s, 0, sizeof *s);
                rc = ZS_E_SIGN;
                goto done;
            }
            s->pool = (uint8_t)ZS_POOL(p);
            s->index = (uint32_t)i;
            n++;
        }
    }
    if (n > max) { rc = ZS_E_TOO_MANY; goto done; }
    *n_out = n;
done:
    memzero(ask, sizeof ask);
    memzero(mine, sizeof mine);
    memzero(&fvk, sizeof fvk);
    memzero(&scratch, sizeof scratch);
    (void)account;
    return rc;
}

int32_t zc_pczt_verify_sig(zc_ctx *ctx, const zs_sig *sig)
{
    static zc_fvk f;
    if (!ctx || !sig) return ZS_ERR_ARG;
    if (sig->pool != ZS_POOL_ORCHARD && sig->pool != ZS_POOL_IRONWOOD) return ZS_ERR_ARG;
    int32_t rc = prepare(ctx);
    if (rc != 0) return rc;
    int p = sig->pool == ZS_POOL_ORCHARD ? 0 : 1;
    const zc_obundle *b = bundle_of(&ctx->z, p);
    if (sig->index >= b->n) return ZS_E_SIGN;
    const zc_action *a = &ctx->z.actions[b->first + sig->index];
    /* As apply_*_signature: verify_nullifier(None) first (the spend's own
     * FVK must be present and, when the note is complete, own it). */
    if (!a->fvk) return ZS_E_SIGN;
    if (a->s_recipient && a->has_s_value && a->rho && a->s_rseed) {
        if (zc_fvk_from_bytes(&f, a->fvk) != 0) return ZS_E_SIGN;
        rc = verify_nullifier(a, &f, note_version_of(p));
        memzero(&f, sizeof f);
        if (rc != 0) return ZS_E_SIGN;
    }
    return zc_redpallas_verify(a->rk, ctx->sighash, sig->sig) == 0 ? 0 : ZS_E_SIGN;
}

int32_t zc_account_derive(const uint8_t seed[64], uint32_t account, uint8_t net,
                          const uint8_t *t_account, const uint8_t *t_receiver,
                          zs_account *out)
{
    static zc_fvk f;
    uint8_t ask[32];
    if (!seed || !out) return ZS_ERR_ARG;
    if (net != ZS_NET_MAIN && net != ZS_NET_TEST) return ZS_ERR_ARG;
    if (zc_crypto_init() != 0) return ZS_ERR_DERIVE;
    memset(out, 0, sizeof *out);
    zc_seed_fingerprint(seed, out->seed_fingerprint);
    if (zc_account_keys(seed, net == ZS_NET_MAIN ? 133 : 1, account, ask, &f) != 0)
        return ZS_ERR_DERIVE;
    memzero(ask, sizeof ask);
    memcpy(out->orchard_fvk, f.ak, 32);
    memcpy(out->orchard_fvk + 32, f.nk, 32);
    memcpy(out->orchard_fvk + 64, f.rivk, 32);
    zc_fvk_default_address(&f, out->orchard_address);
    memzero(&f, sizeof f);
    if (!zc_encode_ufvk(out->ufvk, sizeof out->ufvk, net, out->orchard_fvk, t_account))
        return ZS_ERR_ENCODE;
    if (!zc_encode_ua(out->ua, sizeof out->ua, net, out->orchard_address, t_receiver))
        return ZS_ERR_ENCODE;
    return ZS_OK;
}

int32_t zc_selftest(void)
{
    return zc_crypto_selftest() == 0 ? 0 : ZS_E_INTERNAL;
}
