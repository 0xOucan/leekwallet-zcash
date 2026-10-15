/**
 * `.zkv`: a seed on an SD card, sealed under a passphrase (roadmap 1.4).
 *
 * The card is the storage and the passphrase is the key; the device keeps
 * nothing. A card can be copied, so there is no retry counter to lean on:
 * the protection is the passphrase's entropy times the KDF's work factor,
 * and both travel with the file so it opens on any device that reads the
 * format. Byte layout and rationale: docs/zcash/FORMATS.md.
 *
 *   header (38 bytes, authenticated as GCM associated data)
 *     0  4  magic "LKZV"
 *     4  1  format version (1)
 *     5  1  KDF family (1 = PBKDF2-HMAC-SHA512)
 *     6  4  iterations, u32 LE
 *    10 16  salt
 *    26 12  nonce
 *   ciphertext (AES-256-GCM of the payload) || tag (16)
 *
 *   payload v1
 *     0  1  payload version (1)
 *     1  1  entropy length (16, 20, 24, 28 or 32: a 12-24 word BIP39 phrase)
 *     2  L  BIP39 entropy
 *     .  4  birthday height, u32 LE
 *     .  4  ZIP 32 account, u32 LE (< 2^31)
 *     .  1  flags (bit 0: use_qsk; the rest must be zero)
 *     .  1  label length (0-32)
 *     .  N  label, printable ASCII
 *
 * Entropy rather than the 64-byte seed: the device's own paths start from a
 * phrase (and a BIP39 passphrase, entered separately as for any other
 * wallet), so an SD seed loads exactly as a typed one does and every storage
 * mode gives the same keys for the same phrase.
 *
 * Pure: no I/O, no ESP-IDF. The host suite checks it, and zecz reads and
 * writes the same bytes from an independent Rust implementation.
 */
#ifndef LEEK_ZKV_H
#define LEEK_ZKV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ZKV_MAGIC            "LKZV"
#define ZKV_VERSION          1
#define ZKV_KDF_PBKDF2_SHA512 1

#define ZKV_HEADER_SIZE      38
#define ZKV_TAG_SIZE         16
#define ZKV_LABEL_MAX        32
#define ZKV_ENTROPY_MAX      32
#define ZKV_PAYLOAD_MAX      (1 + 1 + ZKV_ENTROPY_MAX + 4 + 4 + 1 + 1 + ZKV_LABEL_MAX)
#define ZKV_FILE_MAX         (ZKV_HEADER_SIZE + ZKV_PAYLOAD_MAX + ZKV_TAG_SIZE)

/* Work factor. The default costs a few seconds on the S3, once per load;
 * the floor refuses a file someone sealed cheaply elsewhere, and the ceiling
 * refuses one that would hold the device for minutes. */
#define ZKV_ITERATIONS_DEFAULT 20000u
#define ZKV_ITERATIONS_MIN     10000u
#define ZKV_ITERATIONS_MAX     200000u

/* The passphrase: printable ASCII, so no normalisation can differ between
 * the device's text entry and a companion's keyboard. */
#define ZKV_PASS_MIN 8
#define ZKV_PASS_MAX 64

#define ZKV_FLAG_USE_QSK 0x01

#define ZKV_OK          0
#define ZKV_E_ARG     (-1)   /* bad argument, or contents that cannot be sealed */
#define ZKV_E_FORMAT  (-2)   /* not a .zkv file, or truncated */
#define ZKV_E_VERSION (-3)   /* a format or KDF this firmware does not know */
#define ZKV_E_KDF     (-4)   /* iterations outside the accepted range */
#define ZKV_E_AUTH    (-5)   /* wrong passphrase, or the file was altered */
#define ZKV_E_PAYLOAD (-6)   /* authentic, but the payload is malformed */
#define ZKV_E_PASS    (-7)   /* passphrase too short, too long or not printable ASCII */

typedef struct {
    uint8_t  entropy[ZKV_ENTROPY_MAX];
    uint8_t  entropy_len;
    uint32_t birthday;
    uint32_t account;
    uint8_t  flags;
    char     label[ZKV_LABEL_MAX + 1];
} ZkvContents;

/**
 * Seal `c` under `pass` with a fresh salt and nonce. On success `*out_len`
 * is the file size (at most ZKV_FILE_MAX).
 */
int zkv_seal(const ZkvContents *c, const char *pass, size_t pass_len, uint32_t iterations,
             uint8_t *out, size_t cap, size_t *out_len);

/**
 * Open a file. Header checks come first and cost nothing; the KDF runs only
 * for a well-formed header with an accepted work factor. On any failure `out`
 * is zeroed.
 */
int zkv_open(const uint8_t *file, size_t len, const char *pass, size_t pass_len,
             ZkvContents *out);

/** Check the header alone; `*iterations` (optional) is its work factor. */
int zkv_peek(const uint8_t *file, size_t len, uint32_t *iterations);

/** True if `pass` is acceptable as a .zkv passphrase. */
bool zkv_pass_ok(const char *pass, size_t pass_len);

/** Short human-readable reason for a ZKV_E_* code. */
const char *zkv_strerror(int code);

#ifdef LEEK_HOST_TEST
/** Seal with a given salt and nonce, for fixed test vectors only. */
int zkv__seal_fixed_for_test(const ZkvContents *c, const char *pass, size_t pass_len,
                             uint32_t iterations, const uint8_t salt[16],
                             const uint8_t nonce[12], uint8_t *out, size_t cap,
                             size_t *out_len);
#endif

#endif /* LEEK_ZKV_H */
