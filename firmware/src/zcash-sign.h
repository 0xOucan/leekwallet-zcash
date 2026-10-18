/**
 * PCZT intake and signing (roadmap phases 3.1, 3.5 and 4.3).
 *
 * A host uploads one PCZT in pieces (zSignBegin / zSignChunk / zSignFinish in
 * protocol.c). This module owns the bytes from the first chunk to the last
 * signature: it holds them in one buffer (PSRAM where there is some), checks
 * the declared SHA-256 before any parser sees them, runs the signer backends
 * on the Zcash worker, and signs only after the caller says the user
 * approved the summary that the check produced.
 *
 * Backends. With BOTH (the default where both are built), the Rust and C
 * signers check the same bytes independently and their summaries, sighash
 * included, must be byte-identical; any difference is a refusal. The Rust
 * backend then signs and every signature must verify under both backends
 * before it leaves the device.
 *
 * One upload at a time. zcash_sign_begin() discards whatever was there.
 */
#ifndef LEEK_ZCASH_SIGN_H
#define LEEK_ZCASH_SIGN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "zsign_rust.h"

/* Largest PCZT accepted. A two-input shielded send is ~10 KB; this leaves
 * room for many actions and stays a small slice of the 8 MB PSRAM. */
#define ZCASH_PCZT_MAX (512u * 1024u)

/* Errors of this layer. The signers' own refusals (ZS_E_*, -10..-23) pass
 * through unchanged. */
#define ZSIGN_E_STATE      (-30)   /* call out of order */
#define ZSIGN_E_TOO_LARGE  (-31)   /* declared length 0 or above ZCASH_PCZT_MAX */
#define ZSIGN_E_OFFSET     (-32)   /* chunk not at the next expected offset, or past the end */
#define ZSIGN_E_HASH       (-33)   /* bytes do not hash to the declared SHA-256 */
#define ZSIGN_E_INCOMPLETE (-34)   /* finish before every byte arrived */
#define ZSIGN_E_DISAGREE   (-35)   /* BOTH: the backends disagree */
#define ZSIGN_E_NOMEM      (-36)
#define ZSIGN_E_WORKER     (-37)   /* the Zcash worker did not run the job */
#define ZSIGN_E_LOCKED     (-38)   /* no seed available (device locked) */
#define ZSIGN_E_VERIFY     (-39)   /* a fresh signature failed verification */
/* The request names the other network than the board's own setting
 * (Settings > Network). Raised by protocol.c for every Zcash request that
 * names a network, before anything is derived: a board set to one network
 * answers for that network only. */
#define ZSIGN_E_BOARD_NETWORK (-40)

/* Consensus branch IDs both backends accept (each checks its own copy:
 * zsign-rust ALLOWED_BRANCHES, zsign-c BRANCH_NU6_3/BRANCH_NU7; the host suite
 * checks this list against both). Reported by zGetFeatures. */
#define ZCASH_SIGN_BRANCH_COUNT 2u
static const uint32_t ZCASH_SIGN_BRANCHES[ZCASH_SIGN_BRANCH_COUNT] = { 0x37a5165bu, 0x77190ad9u };

typedef enum {
    ZSIGN_BACKEND_RUST = 0,
    ZSIGN_BACKEND_C    = 1,
    ZSIGN_BACKEND_BOTH = 2,
} ZcashSignBackend;

void             zcash_sign_set_backend(ZcashSignBackend b);
ZcashSignBackend zcash_sign_backend(void);
const char      *zcash_sign_backend_name(ZcashSignBackend b);

/** Start an upload of `len` bytes that must hash to `sha256`. */
int zcash_sign_begin(uint32_t len, const uint8_t sha256[32], uint32_t account, uint8_t network);

/** Append `n` bytes at `offset`, which must equal the bytes received so far. */
int zcash_sign_chunk(uint32_t offset, const uint8_t *data, size_t n);

/** The network the current upload was declared for (ZS_NET_*). */
uint8_t zcash_sign_network(void);

/** Bytes received so far in the current upload. */
uint32_t zcash_sign_received(void);

/**
 * Verify the hash and run every check. On success `*out` points at the
 * summary the user must review; it stays valid until zcash_sign_reset().
 */
int zcash_sign_check(const zs_summary **out);

/**
 * Sign every spend of ours. Only after zcash_sign_check() succeeded and the
 * user approved; the upload is consumed either way.
 */
int zcash_sign_sign(zs_sig *sigs, uint32_t max, uint32_t *n_out);

/* A transparent signature as it leaves the device: DER, then the sighash
 * type byte (always SIGHASH_ALL). 9..73 bytes. */
#define ZCASH_TSIG_MAX 73

/**
 * Encode a pool 0 signature's r || s as DER || 0x01 (FORMATS.md). Returns
 * the length, 0 if `s` is not a transparent signature.
 */
size_t zcash_tsig_encode(const zs_sig *s, uint8_t out[ZCASH_TSIG_MAX]);

/** Drop the upload and anything derived from it. */
void zcash_sign_reset(void);

/** Short human-readable reason for any code above or any ZS_E_* code. */
const char *zcash_sign_strerror(int code);

#ifdef LEEK_HOST_TEST
/** Flip one byte of the C backend's summary after its check: a disagreement. */
void zcash_sign__inject_disagreement_for_test(bool on);
#endif

#endif /* LEEK_ZCASH_SIGN_H */
