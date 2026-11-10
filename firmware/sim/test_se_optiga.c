/**
 * OPTIGA Trust M bring-up (components/leek-se-optiga), everything short of the
 * chip itself.
 *
 * Three kinds of check, and it matters which is which:
 *
 *   1. Against outside references. The CRC is pinned to the catalogued
 *      CRC-16/KERMIT check value and to avr-libc's published
 *      _crc_ccitt_update() formula, which is the byte-at-a-time form of the
 *      same function Infineon's reference code uses. The OpenApplication
 *      APDU is pinned to the Trust M application identifier as Infineon
 *      publishes it. These would catch a misreading of the spec.
 *
 *   2. Against bytes written down here by hand (control frames, command
 *      APDUs). These catch a change in what the code emits.
 *
 *   3. Against a simulated chip (below). The simulation is a second reading
 *      of the same spec by the same author, so agreement proves the link
 *      layer handles chaining, ACKs, retries, stale responses and faults
 *      consistently - NOT that a real Trust M behaves this way. Only the
 *      bench build on a board can show that; see the component README.
 *
 * The simulation also enforces the bring-up's one hard rule: it records
 * every command code it receives and every register written, and the suite
 * fails if anything other than OpenApplication, GetDataObject, GetRandom, the
 * warm reset and frames ever reaches it.
 *
 * The same file is built twice (sim/Makefile). test_se_optiga is the default
 * build, where the rule above is absolute and the container's write path must
 * not even compile. test_se_container is built with
 * -DLEEK_SE_CONTAINER_WRITES=1: it runs every check above unchanged, then the
 * Level 1 container's (optiga-container.h), against a simulated 0xF1E0 with
 * factory metadata. There the rule widens by exactly one command:
 * SetDataObject, data only (Param 0x00 or 0x40), on 0xF1E0 only. Anything
 * else - another object, metadata, a counter, any other command - is counted
 * as forbidden and fails the suite.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "optiga-proto.h"
#include "optiga-link.h"
#include "optiga-cmd.h"
#include "optiga-container.h"

/* The default build has no write path to test, and must not have one to
   call: these names exist only when the flag is on. */
#if !LEEK_SE_CONTAINER_WRITES
#  if defined(OPTIGA_CMD_SET_DATA) || defined(OPTIGA_OID_CONTAINER) || \
      defined(OPTIGA_APDU_SET_MAX) || defined(OPTIGA_CONTAINER_CAPACITY)
#    error "write path visible with LEEK_SE_CONTAINER_WRITES=0"
#  endif
#endif

static int failures = 0;

#define CHECK(cond, ...) do {                        \
    if (!(cond)) {                                   \
        printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
        printf(__VA_ARGS__);                         \
        printf("\n"); failures++;                    \
    }                                                \
} while (0)

/* ------------------------------------------------- reference CRC (avr-libc) */

/* avr-libc <util/crc16.h>, _crc_ccitt_update(), as documented there in C.
   Independent of optiga_crc16's bitwise loop. */
static uint16_t ref_crc_update(uint16_t crc, uint8_t data)
{
    data ^= (uint8_t)(crc & 0xff);
    data ^= (uint8_t)(data << 4);
    return (uint16_t)((((uint16_t)data << 8) | (crc >> 8)) ^
                      (uint8_t)(data >> 4) ^ ((uint16_t)data << 3));
}

static uint16_t ref_crc(const uint8_t *p, size_t n)
{
    uint16_t c = 0;
    for (size_t i = 0; i < n; i++)
        c = ref_crc_update(c, p[i]);
    return c;
}

static void test_crc(void)
{
    printf("crc\n");
    CHECK(optiga_crc16((const uint8_t *)"123456789", 9) == 0x2189,
          "KERMIT check value: got %04X", optiga_crc16((const uint8_t *)"123456789", 9));
    CHECK(optiga_crc16(NULL, 0) == 0, "empty");
    uint8_t buf[300];
    uint32_t s = 12345;
    for (int trial = 0; trial < 200; trial++) {
        size_t n = (size_t)(trial * 7 % 300);
        for (size_t i = 0; i < n; i++) {
            s = s * 1103515245u + 12345u;
            buf[i] = (uint8_t)(s >> 16);
        }
        CHECK(optiga_crc16(buf, n) == ref_crc(buf, n), "agrees with avr-libc at n=%zu", n);
    }
}

/* ------------------------------------------------------------- frames */

static void test_frames(void)
{
    printf("frames\n");
    uint8_t f[OPTIGA_MAX_FRAME];

    /* Control frames, against bytes written out here. The CRC bytes were
       produced by the avr-libc formula above, not by the code under test. */
    struct { uint8_t seq, ack; uint8_t want[5]; } ctl[] = {
        { OPTIGA_SEQ_RESYNC, 0, { 0xC0, 0x00, 0x00, 0, 0 } },
        { OPTIGA_SEQ_ACK,    0, { 0x80, 0x00, 0x00, 0, 0 } },
        { OPTIGA_SEQ_ACK,    3, { 0x83, 0x00, 0x00, 0, 0 } },
        { OPTIGA_SEQ_NAK,    1, { 0xA1, 0x00, 0x00, 0, 0 } },
    };
    for (size_t i = 0; i < sizeof ctl / sizeof ctl[0]; i++) {
        uint16_t c = ref_crc(ctl[i].want, 3);
        ctl[i].want[3] = (uint8_t)(c >> 8);
        ctl[i].want[4] = (uint8_t)c;
        CHECK(optiga_frame_control(f, ctl[i].seq, ctl[i].ack) == 5, "control len");
        CHECK(memcmp(f, ctl[i].want, 5) == 0, "control frame %zu bytes", i);
    }

    /* A data frame: FCTR(frnr 1, acknr 2) | LEN 0x0007 | PCTR | 81 00 00 02 E0 C2 | FCS */
    static const uint8_t apdu[] = { 0x81, 0x00, 0x00, 0x02, 0xE0, 0xC2 };
    size_t n = optiga_frame_data(f, sizeof f, 1, 2, OPTIGA_PCTR_CHAIN_NONE, apdu, sizeof apdu);
    uint8_t want[] = { 0x06, 0x00, 0x07, 0x00, 0x81, 0x00, 0x00, 0x02, 0xE0, 0xC2, 0, 0 };
    uint16_t c = ref_crc(want, 10);
    want[10] = (uint8_t)(c >> 8);
    want[11] = (uint8_t)c;
    CHECK(n == sizeof want && memcmp(f, want, n) == 0, "data frame bytes");

    optiga_frame_t pf;
    CHECK(optiga_frame_parse(f, n, &pf) == OPTIGA_FRAME_OK, "parse own frame");
    CHECK(!pf.control && pf.frnr == 1 && pf.acknr == 2 && pf.pctr == 0 &&
          pf.payload_len == sizeof apdu && memcmp(pf.payload, apdu, sizeof apdu) == 0,
          "parsed fields");

    /* Faults, each on its own. */
    uint8_t bad[sizeof want];
    memcpy(bad, want, sizeof bad);
    bad[5] ^= 0x01;
    CHECK(optiga_frame_parse(bad, sizeof bad, &pf) == OPTIGA_FRAME_CRC, "flipped payload bit");
    memcpy(bad, want, sizeof bad);
    bad[11] ^= 0x80;
    CHECK(optiga_frame_parse(bad, sizeof bad, &pf) == OPTIGA_FRAME_CRC, "flipped FCS bit");
    CHECK(optiga_frame_parse(want, 4, &pf) == OPTIGA_FRAME_SHORT, "short");
    CHECK(optiga_frame_parse(want, sizeof want - 1, &pf) == OPTIGA_FRAME_LENGTH, "truncated");

    uint8_t ctl_body[6] = { 0x80, 0x00, 0x01, 0xAA, 0, 0 };
    c = ref_crc(ctl_body, 4);
    ctl_body[4] = (uint8_t)(c >> 8); ctl_body[5] = (uint8_t)c;
    CHECK(optiga_frame_parse(ctl_body, 6, &pf) == OPTIGA_FRAME_CTRL_LEN, "control with body");

    uint8_t empty[5] = { 0x00, 0x00, 0x00, 0, 0 };
    c = ref_crc(empty, 3);
    empty[3] = (uint8_t)(c >> 8); empty[4] = (uint8_t)c;
    CHECK(optiga_frame_parse(empty, 5, &pf) == OPTIGA_FRAME_EMPTY, "data frame without PCTR");

    /* Size limits. */
    static uint8_t big[OPTIGA_MAX_FRAME];
    CHECK(optiga_frame_data(f, sizeof f, 0, 0, 0, big, OPTIGA_MAX_FRAME - 6) == OPTIGA_MAX_FRAME,
          "largest frame fits");
    CHECK(optiga_frame_data(f, sizeof f, 0, 0, 0, big, OPTIGA_MAX_FRAME - 5) == 0,
          "one byte over is refused");
    CHECK(optiga_frame_data(f, 10, 0, 0, 0, apdu, sizeof apdu) == 0, "caller cap honoured");
}

/* ------------------------------------------------------------- APDUs */

static void test_apdus(void)
{
    printf("apdus\n");
    uint8_t a[OPTIGA_APDU_MAX];

    static const uint8_t open_app[] = {
        0xF0, 0x00, 0x00, 0x10,
        0xD2, 0x76, 0x00, 0x00, 0x04, 0x47, 0x65, 0x6E,
        0x41, 0x75, 0x74, 0x68, 0x41, 0x70, 0x70, 0x6C,
    };
    size_t n = optiga_apdu_open_app(a);
    CHECK(n == sizeof open_app && memcmp(a, open_app, n) == 0, "OpenApplication");

    static const uint8_t get_uid[] = { 0x81, 0x00, 0x00, 0x02, 0xE0, 0xC2 };
    n = optiga_apdu_get_data(a, 0xE0C2, false);
    CHECK(n == 6 && memcmp(a, get_uid, 6) == 0, "GetDataObject data");

    static const uint8_t get_meta[] = { 0x81, 0x01, 0x00, 0x02, 0xE0, 0xE0 };
    n = optiga_apdu_get_data(a, 0xE0E0, true);
    CHECK(n == 6 && memcmp(a, get_meta, 6) == 0, "GetDataObject metadata");

    static const uint8_t get_err[] = { 0x01, 0x00, 0x00, 0x02, 0xF1, 0xC2 };
    n = optiga_apdu_get_error(a);
    CHECK(n == 6 && memcmp(a, get_err, 6) == 0, "error code read does not flush");

    static const uint8_t rnd[] = { 0x8C, 0x00, 0x00, 0x02, 0x00, 0x20 };
    n = optiga_apdu_get_random(a, 32);
    CHECK(n == 6 && memcmp(a, rnd, 6) == 0, "GetRandom 32");
    CHECK(optiga_apdu_get_random(a, 7) == 0, "GetRandom below 8 refused");
    CHECK(optiga_apdu_get_random(a, 257) == 0, "GetRandom above 256 refused");
    CHECK(optiga_apdu_get_random(a, 256) == 6 && a[4] == 0x01 && a[5] == 0x00, "GetRandom 256");

    const uint8_t *d;
    size_t dl;
    static const uint8_t ok[] = { 0x00, 0x00, 0x00, 0x02, 0x07, 0x09 };
    CHECK(optiga_apdu_response(ok, 6, &d, &dl) == OPTIGA_APDU_OK && dl == 2 && d[0] == 7,
          "response ok");
    static const uint8_t ok0[] = { 0x00, 0x00, 0x00, 0x00 };
    CHECK(optiga_apdu_response(ok0, 4, &d, &dl) == OPTIGA_APDU_OK && dl == 0, "empty ok");
    static const uint8_t fail[] = { 0xFF, 0x00, 0x00, 0x00 };
    CHECK(optiga_apdu_response(fail, 4, &d, &dl) == OPTIGA_APDU_FAILED, "Sta FF");
    CHECK(optiga_apdu_response(ok, 5, &d, &dl) == OPTIGA_APDU_LENGTH, "OutLen mismatch");
    CHECK(optiga_apdu_response(ok, 3, &d, &dl) == OPTIGA_APDU_SHORT, "short response");
}

/* ------------------------------------------------------ identity parsing */

/* A UID in the documented layout. Field values are illustrative, chosen to
   make each one distinguishable, not copied from a real chip. */
static const uint8_t SAMPLE_UID[OPTIGA_UID_LEN] = {
    0xCD, 0x16, 0x33,                       /* CIM, platform, model */
    0x82, 0x01,                             /* ROM mask */
    0x00, 0x1C, 0x00, 0x05, 0x00, 0x00,     /* chip type */
    0x0A, 0x09, 0x1B, 0x5C, 0x00, 0x0B,     /* batch */
    0x00, 0x12, 0x00, 0x34,                 /* x, y */
    0x80, 0x10, 0x10, 0x71,                 /* firmware id */
    0x08, 0x09,                             /* ESW build */
};

static void test_uid(void)
{
    printf("uid\n");
    optiga_uid_t u;
    CHECK(optiga_parse_uid(SAMPLE_UID, sizeof SAMPLE_UID, &u), "parse");
    CHECK(u.cim == OPTIGA_CIM_INFINEON && u.platform == 0x16 && u.model == 0x33, "head");
    CHECK(u.rom_mask == 0x8201, "rom mask");
    CHECK(u.chip_type[1] == 0x1C && u.batch[0] == 0x0A && u.batch[5] == 0x0B, "type/batch");
    CHECK(u.x == 0x12 && u.y == 0x34, "coords");
    CHECK(u.fw_id == 0x80101071 && u.esw_build == 0x0809, "fw");
    CHECK(!optiga_parse_uid(SAMPLE_UID, 26, &u), "26 bytes refused");
    CHECK(!optiga_parse_uid(SAMPLE_UID, 28, &u), "28 bytes refused");

    CHECK(strcmp(optiga_lcs_name(0x07), "operational") == 0, "LCS op");
    CHECK(strcmp(optiga_lcs_name(0x0F), "termination") == 0, "LCS te");
    CHECK(strcmp(optiga_lcs_name(0x42), "unknown") == 0, "LCS unknown");
    CHECK(strcmp(optiga_error_name(0x01), "invalid OID") == 0, "err 01");
    CHECK(strcmp(optiga_error_name(0x07), "access conditions not satisfied") == 0, "err 07");
}

/* A fake certificate: DER SEQUENCE with a 2-byte long-form length. */
static size_t make_der(uint8_t *out, size_t body)
{
    out[0] = 0x30; out[1] = 0x82;
    out[2] = (uint8_t)(body >> 8); out[3] = (uint8_t)body;
    for (size_t i = 0; i < body; i++)
        out[4 + i] = (uint8_t)(i * 31 + 7);
    return 4 + body;
}

/* Wrap DER the way Infineon's TLS-identity format does. */
static size_t make_tls_identity(uint8_t *out, const uint8_t *der, size_t n)
{
    size_t chain = n + 3, outer = chain + 3;
    out[0] = 0xC0;
    out[1] = (uint8_t)(outer >> 8); out[2] = (uint8_t)outer;
    out[3] = (uint8_t)(chain >> 16); out[4] = (uint8_t)(chain >> 8); out[5] = (uint8_t)chain;
    out[6] = (uint8_t)(n >> 16); out[7] = (uint8_t)(n >> 8); out[8] = (uint8_t)n;
    memcpy(&out[9], der, n);
    return 9 + n;
}

static void test_cert(void)
{
    printf("cert\n");
    static uint8_t der[700], wrapped[720];
    size_t dn = make_der(der, 500);
    const uint8_t *p;
    size_t pn;

    CHECK(optiga_cert_find(der, dn, &p, &pn) && p == der && pn == dn, "bare DER");
    /* Trailing padding after the DER (objects are often zero-filled) is fine:
       the DER's own length decides. */
    CHECK(optiga_cert_find(der, dn + 20 <= sizeof der ? dn + 20 : dn, &p, &pn) && pn == dn,
          "bare DER with padding");
    CHECK(!optiga_cert_find(der, dn - 1, &p, &pn), "truncated DER refused");

    size_t wn = make_tls_identity(wrapped, der, dn);
    CHECK(optiga_cert_find(wrapped, wn, &p, &pn) && p == wrapped + 9 && pn == dn, "TLS identity");

    wrapped[8] ^= 0x01;     /* cert length off by one */
    CHECK(!optiga_cert_find(wrapped, wn, &p, &pn), "inner length mismatch refused");
    wrapped[8] ^= 0x01;
    wrapped[2] += 1;        /* outer length runs past the object */
    CHECK(!optiga_cert_find(wrapped, wn, &p, &pn), "outer overrun refused");

    static const uint8_t junk[] = { 0x00, 0x00, 0x00 };
    CHECK(!optiga_cert_find(junk, sizeof junk, &p, &pn), "neither form");
    CHECK(!optiga_cert_find(junk, 0, &p, &pn), "empty");
}

/* ----------------------------------------------------------- TRNG check */

static uint64_t xs = 0x9E3779B97F4A7C15ull;
static uint8_t xorshift(void)
{
    xs ^= xs << 13; xs ^= xs >> 7; xs ^= xs << 17;
    return (uint8_t)(xs >> 32);
}

static void test_rng(void)
{
    printf("rng sanity\n");
    uint8_t b[256];
    optiga_rng_report_t r;

    /* A decent generator must pass essentially always: try many draws. */
    int passed = 0;
    for (int t = 0; t < 2000; t++) {
        size_t n = (t % 2) ? 256 : 32;
        for (size_t i = 0; i < n; i++)
            b[i] = xorshift();
        passed += optiga_rng_sanity(b, n, &r);
    }
    CHECK(passed == 2000, "good source passed %d/2000", passed);

    memset(b, 0, sizeof b);
    CHECK(!optiga_rng_sanity(b, 256, &r) && r.why, "all zero fails");
    memset(b, 0xFF, sizeof b);
    CHECK(!optiga_rng_sanity(b, 256, &r), "all ones fails");
    memset(b, 0xA5, sizeof b);
    CHECK(!optiga_rng_sanity(b, 256, &r), "constant balanced byte fails (%s)", r.why ? r.why : "-");
    for (size_t i = 0; i < 256; i++)
        b[i] = xorshift() & 0xFE;   /* one stuck-low bit */
    CHECK(!optiga_rng_sanity(b, 256, &r), "stuck bit fails");
    CHECK(!optiga_rng_sanity(b, 31, &r), "too short refused");
}

/* ======================================================= simulated chip */

/* Enough frames for a 1504-byte response at DATA_REG_LEN 16 (151). */
#define Q_MAX 256

#define SIM_F1E0_SIZE 1500

typedef struct {
    /* registers */
    uint16_t data_reg_len;
    uint8_t pending_reg;
    /* frames waiting for the host */
    uint8_t q[Q_MAX][OPTIGA_MAX_FRAME];
    size_t qlen[Q_MAX];
    int qhead, qtail;
    /* link state */
    uint8_t tx_nr;          /* last frame number the chip sent */
    uint8_t rx_nr;          /* last frame number the chip accepted */
    uint8_t cmd[2048];
    size_t cmd_len;
    /* app state */
    bool app_open;
    uint8_t err;
    uint32_t rng;
    /* fault knobs */
    int nack_writes;        /* NACK this many writes */
    int busy_polls;         /* report BUSY this many state polls per response */
    int busy_left;
    bool skip_ack;          /* answer data without a separate ACK */
    bool corrupt_next;      /* flip a bit in the next response frame */
    bool silent;            /* accept frames, never answer */
    bool stuck_rng;
    bool no_soft_reset;     /* NACK the warm-reset register */
    /* what the host did */
    int soft_resets, resyncs, len_writes, other_reg_writes;
    uint8_t cmds_seen[64];
    int ncmds;
    int host_acks_bad;
    /* every command, not just the first 64 */
    int total_cmds;
    int forbidden;          /* anything outside the allowed set, ever */
    int set_ok;             /* SetDataObject on 0xF1E0 data, accepted */
    int q_overflow;
    /* 0xF1E0, the container object: 1500 bytes, factory metadata */
    uint8_t f1e0[SIM_F1E0_SIZE];
    size_t f1e0_len;
    bool f1e0_locked;       /* refuse writes as a locked object would (0x07) */
    int cut_after_sets;     /* > 0: power is lost on that SetDataObject */
    bool bad_store;         /* accept writes but store the last byte wrong */
} fake_t;

static fake_t chip;

static void fake_reset(uint16_t reg_len)
{
    memset(&chip, 0, sizeof chip);
    chip.data_reg_len = reg_len;
    chip.tx_nr = 3;
    chip.rx_nr = 3;
    chip.rng = 1;
}

static void q_push(const uint8_t *f, size_t n)
{
    if ((chip.qtail + 1) % Q_MAX == chip.qhead)
        chip.q_overflow++;
    memcpy(chip.q[chip.qtail], f, n);
    chip.qlen[chip.qtail] = n;
    chip.qtail = (chip.qtail + 1) % Q_MAX;
}

/* The chip's own frame builder, with the reference CRC. */
static void fake_send_frame(bool control, uint8_t seq, uint8_t pctr,
                            const uint8_t *p, size_t n)
{
    uint8_t f[OPTIGA_MAX_FRAME];
    size_t body = control ? 0 : n + 1;
    if (control) {
        f[0] = (uint8_t)(0x80 | seq | (chip.rx_nr & 3));
    } else {
        chip.tx_nr = (chip.tx_nr + 1) & 3;
        f[0] = (uint8_t)((chip.tx_nr << 2) | (chip.rx_nr & 3));
    }
    f[1] = (uint8_t)(body >> 8); f[2] = (uint8_t)body;
    if (!control) {
        f[3] = pctr;
        memcpy(&f[4], p, n);
    }
    uint16_t c = ref_crc(f, 3 + body);
    f[3 + body] = (uint8_t)(c >> 8);
    f[4 + body] = (uint8_t)c;
    if (chip.corrupt_next && !control) {
        f[3] ^= 0x10;
        chip.corrupt_next = false;
    }
    q_push(f, 5 + body);
}

static size_t fake_cert(uint8_t *out)
{
    static uint8_t der[700];
    size_t dn = make_der(der, 560);
    return make_tls_identity(out, der, dn);
}

static void reply(uint8_t sta, const uint8_t *d, size_t n, uint8_t *out, size_t *on)
{
    out[0] = sta; out[1] = 0;
    out[2] = (uint8_t)(n >> 8); out[3] = (uint8_t)n;
    if (n) memcpy(&out[4], d, n);
    *on = 4 + n;
}

/* Execute a complete command APDU. */
static void fake_execute(uint8_t *out, size_t *on)
{
    const uint8_t *c = chip.cmd;
    if (chip.ncmds < 64)
        chip.cmds_seen[chip.ncmds++] = c[0];
    chip.total_cmds++;
    if (c[0] & 0x80)
        chip.err = 0;
    size_t in_len = ((size_t)c[2] << 8) | c[3];
    if (in_len + 4 != chip.cmd_len) {
        chip.err = 0x04;
        reply(0xFF, NULL, 0, out, on);
        return;
    }
    uint8_t code = c[0] & 0x7F;
    if (code == 0x70) {
        static const uint8_t aid[16] = { 0xD2, 0x76, 0x00, 0x00, 0x04, 'G', 'e', 'n',
                                         'A', 'u', 't', 'h', 'A', 'p', 'p', 'l' };
        if (in_len != 16 || memcmp(&c[4], aid, 16) != 0) {
            chip.err = 0x03;
            reply(0xFF, NULL, 0, out, on);
            return;
        }
        chip.app_open = true;
        reply(0x00, NULL, 0, out, on);
        return;
    }
    if (!chip.app_open) {
        chip.err = 0x0B;
        reply(0xFF, NULL, 0, out, on);
        return;
    }
    if (code == 0x01 && in_len == 2) {
        uint16_t oid = (uint16_t)((c[4] << 8) | c[5]);
        static uint8_t obj[2048];
        size_t n = 0;
        if (c[1] == 0x01 && oid == 0xF1E0) {
            /* Factory metadata of an arbitrary data object on a V3 part:
               LcsO creation, max 1500, used size, change LcsO<op, read
               Always. */
            const uint8_t meta[] = {
                0x20, 0x13, 0xC0, 0x01, 0x01, 0xC4, 0x02, 0x05, 0xDC,
                0xC5, 0x02, (uint8_t)(chip.f1e0_len >> 8), (uint8_t)chip.f1e0_len,
                0xD0, 0x03, 0xE1, 0xFC, 0x07, 0xD1, 0x01, 0x00,
            };
            memcpy(obj, meta, sizeof meta); n = sizeof meta;
        } else if (c[1] == 0x01) {
            static const uint8_t meta[] = { 0x20, 0x06, 0xC0, 0x01, 0x07, 0xD1, 0x01, 0x00 };
            memcpy(obj, meta, sizeof meta); n = sizeof meta;
        } else if (oid == 0xF1E0) {
            memcpy(obj, chip.f1e0, chip.f1e0_len); n = chip.f1e0_len;
        } else {
            switch (oid) {
            case 0xE0C2: memcpy(obj, SAMPLE_UID, sizeof SAMPLE_UID); n = sizeof SAMPLE_UID; break;
            case 0xE0C0: case 0xF1C0: obj[0] = 0x07; n = 1; break;
            case 0xE0C5: obj[0] = 0x00; n = 1; break;
            case 0xE0C6: obj[0] = 0x06; obj[1] = 0x15; n = 2; break;
            case 0xE0E0: n = fake_cert(obj); break;
            case 0xF1C2: obj[0] = chip.err; n = 1; break;
            default:
                chip.err = 0x01;
                reply(0xFF, NULL, 0, out, on);
                return;
            }
        }
        reply(0x00, obj, n, out, on);
        return;
    }
    if (code == 0x02) {
        /* SetDataObject: OID(2) | Offset(2) | Data. Modelled so the
           container can be exercised, and policed so nothing else can. */
        if (in_len < 5) {
            chip.forbidden++;
            chip.err = 0x04;
            reply(0xFF, NULL, 0, out, on);
            return;
        }
        uint16_t oid = (uint16_t)((c[4] << 8) | c[5]);
        size_t off = ((size_t)c[6] << 8) | c[7];
        size_t dn = in_len - 4;
        if (oid != 0xF1E0 || (c[1] != 0x00 && c[1] != 0x40)) {
            /* Another object, metadata (0x01), a counter (0x02), anything. */
            chip.forbidden++;
            chip.err = 0x07;
            reply(0xFF, NULL, 0, out, on);
            return;
        }
        if (chip.cut_after_sets > 0 && --chip.cut_after_sets == 0) {
            /* Power lost as the command arrived: nothing applied, no answer. */
            *on = 0;
            return;
        }
        if (chip.f1e0_locked) {
            chip.err = 0x07;
            reply(0xFF, NULL, 0, out, on);
            return;
        }
        if (c[1] == 0x40) {
            memset(chip.f1e0, 0, sizeof chip.f1e0);
            chip.f1e0_len = 0;
        }
        if (off > chip.f1e0_len || off + dn > SIM_F1E0_SIZE) {
            chip.err = 0x08;
            reply(0xFF, NULL, 0, out, on);
            return;
        }
        memcpy(&chip.f1e0[off], &c[8], dn);
        if (chip.bad_store)
            chip.f1e0[off + dn - 1] ^= 0x01;
        if (off + dn > chip.f1e0_len)
            chip.f1e0_len = off + dn;
        chip.set_ok++;
        reply(0x00, NULL, 0, out, on);
        return;
    }
    if (code == 0x0C && in_len == 2) {
        size_t n = ((size_t)c[4] << 8) | c[5];
        uint8_t r[256];
        for (size_t i = 0; i < n; i++) {
            chip.rng = chip.rng * 1664525u + 1013904223u;
            r[i] = chip.stuck_rng ? 0x00 : (uint8_t)(chip.rng >> 24);
        }
        reply(0x00, r, n, out, on);
        return;
    }
    chip.forbidden++;
    chip.err = 0x0A;
    reply(0xFF, NULL, 0, out, on);
}

static void fake_respond(void)
{
    static uint8_t out[2100];
    size_t on = 0;
    fake_execute(out, &on);
    if (on == 0)
        return;     /* the chip lost power mid-command */
    size_t per = (size_t)chip.data_reg_len - 6;
    if (per > OPTIGA_MAX_FRAME - 6) per = OPTIGA_MAX_FRAME - 6;
    size_t off = 0;
    bool first = true;
    while (off < on || first) {
        size_t n = on - off > per ? per : on - off;
        bool last = off + n == on;
        uint8_t pctr = first ? (last ? 0x00 : 0x01) : (last ? 0x04 : 0x02);
        fake_send_frame(false, 0, pctr, out + off, n);
        off += n;
        first = false;
    }
    chip.busy_left = chip.busy_polls;
}

static void fake_frame_in(const uint8_t *f, size_t n)
{
    if (n < 5 || ref_crc(f, n - 2) != (uint16_t)((f[n - 2] << 8) | f[n - 1])) {
        fake_send_frame(true, 0x20, 0, NULL, 0);    /* NAK */
        return;
    }
    if (f[0] & 0x80) {
        uint8_t seq = f[0] & 0x60;
        if (seq == 0x40) {
            chip.resyncs++;
            chip.tx_nr = 3; chip.rx_nr = 3; chip.cmd_len = 0;
        } else if (seq == 0x00) {
            /* The host acknowledges our latest frame, or an earlier one of a
               chain still in the queue; anything else is a host bug. */
            if ((f[0] & 3) > 3) chip.host_acks_bad++;
        }
        return;
    }
    uint8_t frnr = (f[0] >> 2) & 3;
    if (frnr != ((chip.rx_nr + 1) & 3)) {
        chip.host_acks_bad++;
        return;
    }
    chip.rx_nr = frnr;
    size_t body = ((size_t)f[1] << 8) | f[2];
    uint8_t pctr = f[3];
    memcpy(chip.cmd + chip.cmd_len, &f[4], body - 1);
    chip.cmd_len += body - 1;
    uint8_t chain = pctr & 7;
    if (!chip.skip_ack || chain == 0x01 || chain == 0x02)
        fake_send_frame(true, 0x00, 0, NULL, 0);    /* ACK */
    if (chain == 0x00 || chain == 0x04) {
        if (!chip.silent)
            fake_respond();
        chip.cmd_len = 0;
    }
}

static int fake_write(void *ctx, const uint8_t *buf, size_t n)
{
    (void)ctx;
    if (chip.nack_writes > 0) {
        chip.nack_writes--;
        return -1;
    }
    if (n == 1) {
        chip.pending_reg = buf[0];
        return 0;
    }
    switch (buf[0]) {
    case OPTIGA_REG_DATA:
        fake_frame_in(buf + 1, n - 1);
        return 0;
    case OPTIGA_REG_SOFT_RESET: {
        if (chip.no_soft_reset)
            return -1;
        chip.soft_resets++;
        uint16_t keep = chip.data_reg_len;
        chip.qhead = chip.qtail = 0;
        chip.tx_nr = chip.rx_nr = 3;
        chip.app_open = false;
        chip.cmd_len = 0;
        chip.data_reg_len = keep;
        return 0;
    }
    case OPTIGA_REG_DATA_LEN:
        chip.len_writes++;
        return 0;
    default:
        chip.other_reg_writes++;
        return 0;
    }
}

static int fake_read(void *ctx, uint8_t *buf, size_t n)
{
    (void)ctx;
    switch (chip.pending_reg) {
    case OPTIGA_REG_I2C_STATE:
        if (n != 4) return -1;
        memset(buf, 0, 4);
        if (chip.busy_left > 0) {
            chip.busy_left--;
            buf[0] = OPTIGA_STATE_BUSY;
        } else if (chip.qhead != chip.qtail) {
            buf[0] = OPTIGA_STATE_RESP_RDY;
            buf[2] = (uint8_t)(chip.qlen[chip.qhead] >> 8);
            buf[3] = (uint8_t)chip.qlen[chip.qhead];
        }
        return 0;
    case OPTIGA_REG_DATA:
        if (chip.qhead == chip.qtail || n != chip.qlen[chip.qhead]) return -1;
        memcpy(buf, chip.q[chip.qhead], n);
        chip.qhead = (chip.qhead + 1) % Q_MAX;
        return 0;
    case OPTIGA_REG_DATA_LEN:
        if (n != 2) return -1;
        buf[0] = (uint8_t)(chip.data_reg_len >> 8);
        buf[1] = (uint8_t)chip.data_reg_len;
        return 0;
    default:
        return -1;
    }
}

static uint32_t slept;
static void fake_delay(void *ctx, uint32_t ms) { (void)ctx; slept += ms; }

static const optiga_bus_t FAKE_BUS = { NULL, fake_write, fake_read, fake_delay };

/* Every command the host ever sent, checked against the read-only set. */
static void check_read_only(const char *what)
{
    for (int i = 0; i < chip.ncmds; i++) {
        uint8_t c = chip.cmds_seen[i];
        CHECK(c == 0xF0 || c == 0x81 || c == 0x01 || c == 0x8C,
              "%s: command 0x%02X is not read-only", what, c);
    }
    CHECK(chip.len_writes == 0 && chip.other_reg_writes == 0,
          "%s: unexpected register writes", what);
    CHECK(chip.forbidden == 0 && chip.set_ok == 0,
          "%s: %d forbidden command(s), %d write(s)", what, chip.forbidden, chip.set_ok);
    CHECK(chip.q_overflow == 0, "%s: simulated queue overflowed", what);
}

static optiga_link_t link_;

static void test_link_happy(uint16_t reg_len)
{
    printf("link: happy path, DATA_REG_LEN=%u\n", reg_len);
    fake_reset(reg_len);
    optiga_link_t *l = &link_;
    CHECK(optiga_link_init(l, &FAKE_BUS) == OPTIGA_OK, "init");
    CHECK(chip.soft_resets == 1 && chip.resyncs == 1, "warm reset and resync");
    CHECK(l->max_frame == (reg_len < OPTIGA_MAX_FRAME ? reg_len : OPTIGA_MAX_FRAME),
          "max frame %u", l->max_frame);

    uint8_t e = 0xEE;
    CHECK(optiga_cmd_open_app(l, &e) == OPTIGA_OK, "open app (err %02X)", e);

    static uint8_t buf[2048];
    size_t n = 0;
    CHECK(optiga_cmd_get_data(l, OPTIGA_OID_COPROC_UID, false, buf, sizeof buf, &n, &e) == OPTIGA_OK,
          "uid");
    CHECK(n == OPTIGA_UID_LEN && memcmp(buf, SAMPLE_UID, n) == 0, "uid bytes");

    CHECK(optiga_cmd_get_data(l, OPTIGA_OID_LCS_GLOBAL, false, buf, sizeof buf, &n, &e) == OPTIGA_OK &&
          n == 1 && buf[0] == 0x07, "LcsG");

    /* The certificate: 9 + 564 bytes, which chains at every frame size here. */
    CHECK(optiga_cmd_get_data(l, OPTIGA_OID_DEVICE_CERT, false, buf, sizeof buf, &n, &e) == OPTIGA_OK,
          "cert");
    const uint8_t *der; size_t dn;
    CHECK(n == 573 && optiga_cert_find(buf, n, &der, &dn) && dn == 564, "cert unwrap (n=%zu)", n);

    CHECK(optiga_cmd_get_data(l, OPTIGA_OID_DEVICE_CERT, true, buf, sizeof buf, &n, &e) == OPTIGA_OK &&
          n == 8 && buf[0] == 0x20, "metadata");

    uint8_t r1[256], r2[256];
    optiga_rng_report_t rep;
    CHECK(optiga_cmd_get_random(l, r1, 256, &e) == OPTIGA_OK, "random 256");
    CHECK(optiga_cmd_get_random(l, r2, 256, &e) == OPTIGA_OK, "random 256 again");
    CHECK(memcmp(r1, r2, 256) != 0, "two draws differ");
    CHECK(optiga_rng_sanity(r1, 256, &rep), "sim RNG passes sanity");

    /* Refusal, and the reason fetched without being flushed. */
    CHECK(optiga_cmd_get_data(l, 0xE0FF, false, buf, sizeof buf, &n, &e) == OPTIGA_E_CHIP && e == 0x01,
          "unknown OID refused with 0x01 (got %02X)", e);
    /* And the link is still in step afterwards. */
    CHECK(optiga_cmd_get_data(l, OPTIGA_OID_SEC, false, buf, sizeof buf, &n, &e) == OPTIGA_OK &&
          n == 1, "still in step after a refusal");

    CHECK(chip.host_acks_bad == 0, "frame numbering consistent");
    check_read_only("happy");
}

static void test_link_faults(void)
{
    optiga_link_t *l = &link_;
    static uint8_t buf[2048];
    size_t n;
    uint8_t e;

    printf("link: stale response from before a reboot\n");
    fake_reset(64);
    /* Leave a response in the chip's queue as an ESP32 reset mid-command
       would. With no reset pin, nothing else would clear it. */
    chip.app_open = true;
    memcpy(chip.cmd, (const uint8_t[]){ 0x81, 0, 0, 2, 0xE0, 0xC2 }, 6);
    chip.cmd_len = 6;
    fake_respond();
    CHECK(chip.qhead != chip.qtail, "stale frame queued");
    /* And a chip that does not take the warm reset, so the drain is what
       has to clear it. */
    chip.no_soft_reset = true;
    CHECK(optiga_link_init(l, &FAKE_BUS) == OPTIGA_OK, "init");
    CHECK(optiga_cmd_open_app(l, &e) == OPTIGA_OK, "open after stale");
    CHECK(optiga_cmd_get_data(l, OPTIGA_OID_COPROC_UID, false, buf, sizeof buf, &n, &e) == OPTIGA_OK &&
          n == OPTIGA_UID_LEN, "read after stale");

    printf("link: NACKs while busy are retried\n");
    fake_reset(277);
    CHECK(optiga_link_init(l, &FAKE_BUS) == OPTIGA_OK, "init");
    chip.nack_writes = 5;
    CHECK(optiga_cmd_open_app(l, &e) == OPTIGA_OK, "open through 5 NACKs");
    chip.nack_writes = 1000;
    CHECK(optiga_cmd_open_app(l, &e) == OPTIGA_E_BUS, "permanent NACK reported");

    printf("link: BUSY polls\n");
    fake_reset(277);
    CHECK(optiga_link_init(l, &FAKE_BUS) == OPTIGA_OK, "init");
    chip.busy_polls = 200;
    chip.busy_left = 0;
    CHECK(optiga_cmd_open_app(l, &e) == OPTIGA_OK, "open");
    CHECK(optiga_cmd_get_data(l, OPTIGA_OID_DEVICE_CERT, false, buf, sizeof buf, &n, &e) == OPTIGA_OK,
          "cert while busy between frames");

    printf("link: data without a separate ACK\n");
    fake_reset(64);
    chip.skip_ack = true;
    CHECK(optiga_link_init(l, &FAKE_BUS) == OPTIGA_OK, "init");
    CHECK(optiga_cmd_open_app(l, &e) == OPTIGA_OK, "open");
    CHECK(optiga_cmd_get_data(l, OPTIGA_OID_DEVICE_CERT, false, buf, sizeof buf, &n, &e) == OPTIGA_OK &&
          n == 573, "cert");

    printf("link: command chaining at DATA_REG_LEN=16\n");
    fake_reset(16);
    CHECK(optiga_link_init(l, &FAKE_BUS) == OPTIGA_OK, "init");
    /* OpenApplication is 20 bytes; at 10 per packet it goes out in two. */
    CHECK(optiga_cmd_open_app(l, &e) == OPTIGA_OK, "chained open");
    CHECK(optiga_cmd_get_data(l, OPTIGA_OID_COPROC_UID, false, buf, sizeof buf, &n, &e) == OPTIGA_OK &&
          n == OPTIGA_UID_LEN, "uid in 10-byte packets");

    printf("link: corrupted response\n");
    fake_reset(277);
    CHECK(optiga_link_init(l, &FAKE_BUS) == OPTIGA_OK, "init");
    chip.corrupt_next = true;
    CHECK(optiga_cmd_open_app(l, &e) == OPTIGA_E_FRAME, "bad CRC reported");
    /* Recover the way leek-se.c does: re-init. */
    CHECK(optiga_link_init(l, &FAKE_BUS) == OPTIGA_OK, "re-init");
    CHECK(optiga_cmd_open_app(l, &e) == OPTIGA_OK, "open after recovery");

    printf("link: chip never answers\n");
    fake_reset(277);
    CHECK(optiga_link_init(l, &FAKE_BUS) == OPTIGA_OK, "init");
    chip.silent = true;
    /* Idle-with-nothing-ready is reported quickly, not after the full 6 s
       busy deadline: a chip that lost the command is not a slow chip. */
    slept = 0;
    int err = optiga_cmd_open_app(l, &e);
    CHECK(err == OPTIGA_E_PROTOCOL, "silence reported as protocol error (%d)", err);
    CHECK(slept < 100, "and quickly (%u ms)", (unsigned)slept);

    printf("link: chip stays busy forever\n");
    fake_reset(277);
    CHECK(optiga_link_init(l, &FAKE_BUS) == OPTIGA_OK, "init");
    chip.silent = true;
    chip.busy_polls = 1 << 30;
    chip.busy_left = 1 << 30;
    CHECK(optiga_cmd_open_app(l, &e) == OPTIGA_E_TIMEOUT, "busy forever times out");

    printf("link: response bigger than the buffer\n");
    fake_reset(277);
    CHECK(optiga_link_init(l, &FAKE_BUS) == OPTIGA_OK, "init");
    CHECK(optiga_cmd_open_app(l, &e) == OPTIGA_OK, "open");
    CHECK(optiga_cmd_get_data(l, OPTIGA_OID_DEVICE_CERT, false, buf, 100, &n, &e) == OPTIGA_E_SIZE,
          "cap honoured");
    CHECK(optiga_cmd_get_data(l, OPTIGA_OID_LCS_GLOBAL, false, buf, sizeof buf, &n, &e) == OPTIGA_OK,
          "still in step after oversize");

    printf("link: commands before OpenApplication\n");
    fake_reset(277);
    CHECK(optiga_link_init(l, &FAKE_BUS) == OPTIGA_OK, "init");
    CHECK(optiga_cmd_get_data(l, OPTIGA_OID_COPROC_UID, false, buf, sizeof buf, &n, &e) == OPTIGA_E_CHIP,
          "refused before open");

    printf("link: no chip\n");
    fake_reset(277);
    chip.nack_writes = 1 << 30;
    CHECK(optiga_link_init(l, &FAKE_BUS) == OPTIGA_E_BUS, "absent chip");

    printf("link: implausible DATA_REG_LEN\n");
    fake_reset(0xFFFF);
    CHECK(optiga_link_init(l, &FAKE_BUS) == OPTIGA_E_PROTOCOL, "0xFFFF refused");
    fake_reset(4);
    CHECK(optiga_link_init(l, &FAKE_BUS) == OPTIGA_E_PROTOCOL, "4 refused");

    printf("link: stuck TRNG caught\n");
    fake_reset(277);
    chip.stuck_rng = true;
    CHECK(optiga_link_init(l, &FAKE_BUS) == OPTIGA_OK, "init");
    CHECK(optiga_cmd_open_app(l, &e) == OPTIGA_OK, "open");
    uint8_t r[64];
    optiga_rng_report_t rep;
    CHECK(optiga_cmd_get_random(l, r, 64, &e) == OPTIGA_OK, "random");
    CHECK(!optiga_rng_sanity(r, 64, &rep), "stuck output fails sanity");
    CHECK(optiga_cmd_get_random(l, r, 7, &e) == OPTIGA_E_PARAM, "7 bytes refused locally");

    check_read_only("faults");
}

#if LEEK_SE_CONTAINER_WRITES
/* ================================================ Level 1 SE container */

/* The widened rule: SetDataObject on 0xF1E0's data, and nothing else new. */
static void check_container_only(const char *what)
{
    for (int i = 0; i < chip.ncmds; i++) {
        uint8_t c = chip.cmds_seen[i];
        CHECK(c == 0xF0 || c == 0x81 || c == 0x01 || c == 0x8C || c == 0x82,
              "%s: command 0x%02X is outside the container's set", what, c);
    }
    CHECK(chip.forbidden == 0, "%s: %d forbidden command(s)", what, chip.forbidden);
    CHECK(chip.len_writes == 0 && chip.other_reg_writes == 0,
          "%s: unexpected register writes", what);
    CHECK(chip.host_acks_bad == 0, "%s: frame numbering", what);
    CHECK(chip.q_overflow == 0, "%s: simulated queue overflowed", what);
}

static bool link_wiped(const optiga_link_t *l)
{
    for (size_t i = 0; i < sizeof l->tx; i++)
        if (l->tx[i]) return false;
    for (size_t i = 0; i < sizeof l->rx; i++)
        if (l->rx[i]) return false;
    return true;
}

static void fill(uint8_t *p, size_t n, uint32_t seed)
{
    for (size_t i = 0; i < n; i++) {
        seed = seed * 1103515245u + 12345u;
        p[i] = (uint8_t)(seed >> 16);
    }
}

/* A valid container image, built here independently of the code under test. */
static size_t make_container(uint8_t *obj, const uint8_t *p, size_t n)
{
    memset(obj, 0, 16);
    memcpy(obj, "LKSC", 4);
    obj[4] = 1;
    obj[6] = (uint8_t)(n >> 8); obj[7] = (uint8_t)n;
    uint32_t crc = 0xFFFFFFFFu;     /* bytewise CRC-32, written out again */
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    crc = ~crc;
    obj[8] = (uint8_t)(crc >> 24); obj[9] = (uint8_t)(crc >> 16);
    obj[10] = (uint8_t)(crc >> 8); obj[11] = (uint8_t)crc;
    memcpy(&obj[16], p, n);
    return 16 + n;
}

static void test_container_apdus(void)
{
    printf("container: SetDataObject builder\n");
    uint8_t a[OPTIGA_APDU_SET_MAX];
    static const uint8_t d2[] = { 0xAA, 0xBB };

    static const uint8_t want_w[] = { 0x82, 0x00, 0x00, 0x06, 0xF1, 0xE0, 0x00, 0x10, 0xAA, 0xBB };
    size_t n = optiga_apdu_set_data(a, sizeof a, 0xF1E0, false, 0x0010, d2, 2);
    CHECK(n == sizeof want_w && memcmp(a, want_w, n) == 0, "write bytes");
    static const uint8_t want_e[] = { 0x82, 0x40, 0x00, 0x06, 0xF1, 0xE0, 0x00, 0x00, 0xAA, 0xBB };
    n = optiga_apdu_set_data(a, sizeof a, 0xF1E0, true, 0, d2, 2);
    CHECK(n == sizeof want_e && memcmp(a, want_e, n) == 0, "erase-and-write bytes");

    /* Every OID but one, both modes: nothing built. */
    int built = 0;
    for (uint32_t oid = 0; oid <= 0xFFFF; oid++)
        for (int e = 0; e < 2; e++) {
            memset(a, 0xEE, sizeof a);
            size_t m = optiga_apdu_set_data(a, sizeof a, (uint16_t)oid, e, 0, d2, 2);
            if (m) {
                built++;
                CHECK(oid == 0xF1E0, "built for OID %04X", (unsigned)oid);
            } else {
                CHECK(a[0] == 0xEE, "refused OID %04X still wrote", (unsigned)oid);
            }
        }
    CHECK(built == 2, "exactly one OID, two modes (%d)", built);

    /* Whatever the inputs, the command is SetDataObject and the Param is a
       data write: never metadata (0x01), never a counter (0x02). */
    static uint8_t big[OPTIGA_SET_CHUNK_MAX];
    for (size_t len = 0; len <= OPTIGA_SET_CHUNK_MAX + 1; len++)
        for (uint32_t off = 0; off <= 1600; off += 37)
            for (int e = 0; e < 2; e++) {
                size_t m = optiga_apdu_set_data(a, sizeof a, 0xF1E0, e, (uint16_t)off, big, len);
                bool fits = len >= 1 && len <= OPTIGA_SET_CHUNK_MAX && off + len <= 1500;
                CHECK((m != 0) == fits, "len %zu off %u: built=%zu", len, (unsigned)off, m);
                if (m)
                    CHECK(a[0] == 0x82 && (a[1] == 0x00 || a[1] == 0x40) &&
                          a[4] == 0xF1 && a[5] == 0xE0 && m == 8 + len,
                          "shape at len %zu off %u", len, (unsigned)off);
            }
    CHECK(optiga_apdu_set_data(a, sizeof a, 0xF1E0, false, 1499, d2, 1) == 9, "last byte");
    CHECK(optiga_apdu_set_data(a, sizeof a, 0xF1E0, false, 1499, d2, 2) == 0, "past the end");
    CHECK(optiga_apdu_set_data(a, 9, 0xF1E0, false, 0, d2, 2) == 0, "caller cap honoured");
    CHECK(optiga_apdu_set_data(a, sizeof a, 0xF1E0, false, 0, NULL, 2) == 0, "NULL data");
}

static void test_container_format(void)
{
    printf("container: format\n");
    CHECK(optiga_container_crc32((const uint8_t *)"123456789", 9) == 0xCBF43926u,
          "CRC-32 check value");
    CHECK(OPTIGA_CONTAINER_CAPACITY == 1484, "capacity");

    static uint8_t obj[1500], pay[1484];
    const uint8_t *p;
    size_t pl;
    fill(pay, sizeof pay, 7);

    CHECK(optiga_container_parse(obj, 0, &p, &pl) == OPTIGA_CONTAINER_EMPTY, "no bytes");
    memset(obj, 0, sizeof obj);
    CHECK(optiga_container_parse(obj, 1500, &p, &pl) == OPTIGA_CONTAINER_EMPTY, "all zero");

    size_t n = make_container(obj, pay, 100);
    CHECK(optiga_container_parse(obj, n, &p, &pl) == OPTIGA_CONTAINER_OK &&
          pl == 100 && p == obj + 16 && memcmp(p, pay, 100) == 0, "valid");
    /* Zero padding after the payload (an object erased to 1500) is fine. */
    CHECK(optiga_container_parse(obj, 1500, &p, &pl) == OPTIGA_CONTAINER_OK && pl == 100,
          "valid with zero tail");
    n = make_container(obj, pay, 1484);
    CHECK(optiga_container_parse(obj, n, &p, &pl) == OPTIGA_CONTAINER_OK && pl == 1484, "full");
    n = make_container(obj, pay, 0);
    CHECK(optiga_container_parse(obj, n, &p, &pl) == OPTIGA_CONTAINER_OK && pl == 0, "zero-length payload");

    n = make_container(obj, pay, 100);
    obj[16 + 50] ^= 0x01;
    CHECK(optiga_container_parse(obj, n, &p, &pl) == OPTIGA_CONTAINER_DAMAGED && !p && pl == 0,
          "payload bit flip");
    n = make_container(obj, pay, 100);
    obj[5] = 1;
    CHECK(optiga_container_parse(obj, n, &p, &pl) == OPTIGA_CONTAINER_DAMAGED, "flags set");
    n = make_container(obj, pay, 100);
    obj[13] = 1;
    CHECK(optiga_container_parse(obj, n, &p, &pl) == OPTIGA_CONTAINER_DAMAGED, "reserved set");
    n = make_container(obj, pay, 100);
    CHECK(optiga_container_parse(obj, n - 1, &p, &pl) == OPTIGA_CONTAINER_DAMAGED, "truncated");
    CHECK(optiga_container_parse(obj, 10, &p, &pl) == OPTIGA_CONTAINER_DAMAGED, "short header");

    n = make_container(obj, pay, 100);
    memset(obj, 0, 4);
    CHECK(optiga_container_parse(obj, n, &p, &pl) == OPTIGA_CONTAINER_INCOMPLETE, "no magic yet");
    obj[4] = 9;
    CHECK(optiga_container_parse(obj, n, &p, &pl) == OPTIGA_CONTAINER_FOREIGN,
          "zero prefix, not our header");

    n = make_container(obj, pay, 100);
    obj[4] = 2;
    CHECK(optiga_container_parse(obj, n, &p, &pl) == OPTIGA_CONTAINER_FOREIGN, "newer version");
    memcpy(obj, "hello, world", 12);
    CHECK(optiga_container_parse(obj, 12, &p, &pl) == OPTIGA_CONTAINER_FOREIGN, "someone's text");
    CHECK(optiga_container_parse(obj, 2, &p, &pl) == OPTIGA_CONTAINER_FOREIGN, "two bytes");
}

static void open_chip(uint16_t reg_len)
{
    fake_reset(reg_len);
    uint8_t e;
    CHECK(optiga_link_init(&link_, &FAKE_BUS) == OPTIGA_OK, "init");
    CHECK(optiga_cmd_open_app(&link_, &e) == OPTIGA_OK, "open");
}

static void test_container_round_trip(uint16_t reg_len)
{
    printf("container: round trips, DATA_REG_LEN=%u\n", reg_len);
    optiga_link_t *l = &link_;
    open_chip(reg_len);
    static uint8_t data[1600], back[1600];
    size_t n = 99;
    uint8_t e = 0xEE;
    optiga_container_state_t st = OPTIGA_CONTAINER_OK;

    /* A factory object is empty. */
    CHECK(optiga_container_read(l, back, sizeof back, &n, &st, &e) == OPTIGA_OK &&
          st == OPTIGA_CONTAINER_EMPTY && n == 0, "fresh object reads empty");

    size_t sizes[] = { 1, 16, 240, 241, 256, 257, 700, 1484, 33 };
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        size_t len = sizes[i];
        fill(data, len, (uint32_t)(len * 977 + reg_len));
        int err = optiga_container_write(l, data, len, &e);
        CHECK(err == OPTIGA_OK, "write %zu: %s (chip %02X)", len, optiga_container_err_name(err), e);
        CHECK(link_wiped(l), "link buffers wiped after write %zu", len);
        CHECK(chip.f1e0_len == 16 + len, "object holds exactly %zu+16 (has %zu): no stale tail",
              len, chip.f1e0_len);
        memset(back, 0, sizeof back);
        err = optiga_container_read(l, back, sizeof back, &n, &st, &e);
        CHECK(err == OPTIGA_OK && st == OPTIGA_CONTAINER_OK && n == len &&
              memcmp(back, data, len) == 0, "read back %zu (st %d n %zu)", len, st, n);
        CHECK(link_wiped(l), "link buffers wiped after read %zu", len);
    }

    /* Oversize and empty writes go nowhere. */
    int before = chip.total_cmds;
    CHECK(optiga_container_write(l, data, 1485, &e) == OPTIGA_E_PARAM, "1485 refused");
    CHECK(optiga_container_write(l, data, 0, &e) == OPTIGA_E_PARAM, "0 refused");
    CHECK(chip.total_cmds == before, "and nothing reached the chip");
    CHECK(optiga_container_read(l, back, sizeof back, &n, &st, &e) == OPTIGA_OK &&
          st == OPTIGA_CONTAINER_OK && n == 33, "previous blob intact");

    /* A buffer too small for the blob. */
    CHECK(optiga_container_read(l, back, 32, &n, &st, &e) == OPTIGA_E_SIZE && n == 0,
          "read cap honoured");

    /* Erase: zeros over the whole object, then it reads as empty. */
    CHECK(optiga_container_erase(l, &e) == OPTIGA_OK, "erase");
    CHECK(link_wiped(l), "link buffers wiped after erase");
    bool zero = chip.f1e0_len == 1500;
    for (size_t i = 0; i < SIM_F1E0_SIZE; i++)
        zero = zero && chip.f1e0[i] == 0;
    CHECK(zero, "every byte of the object is zero (len %zu)", chip.f1e0_len);
    CHECK(optiga_container_read(l, back, sizeof back, &n, &st, &e) == OPTIGA_OK &&
          st == OPTIGA_CONTAINER_EMPTY && n == 0, "erased object reads empty");
    /* And can be written again. */
    fill(data, 500, 3);
    CHECK(optiga_container_write(l, data, 500, &e) == OPTIGA_OK, "write after erase");

    check_container_only("round trip");
}

static void test_container_refusals(void)
{
    optiga_link_t *l = &link_;
    static uint8_t data[1500], back[1500];
    size_t n;
    uint8_t e;
    optiga_container_state_t st;

    printf("container: other objects never reach the chip\n");
    open_chip(277);
    static const uint16_t others[] = {
        0xF1E1, 0xF1D0, 0xF1D4, 0xF1D8, 0xF1DB, 0xE140, 0xE120, 0xE121, 0xE122,
        0xE0F0, 0xE0F1, 0xE0E0, 0xE0E8, 0xE200, 0xE0C0, 0xF1C0, 0xE0C9, 0x0000, 0xFFFF,
    };
    int before = chip.total_cmds;
    for (size_t i = 0; i < sizeof others / sizeof others[0]; i++)
        for (int er = 0; er < 2; er++)
            CHECK(optiga_cmd_set_data(l, others[i], er, 0, data, 16, &e) == OPTIGA_E_PARAM,
                  "OID %04X refused", others[i]);
    CHECK(chip.total_cmds == before && chip.set_ok == 0, "no command sent for them");

    /* Random calls through the real command path: only 0xF1E0 data writes
       ever arrive, and the simulated chip never sees a forbidden one. */
    uint32_t r = 99;
    int sent = 0;
    for (int i = 0; i < 3000; i++) {
        r = r * 1664525u + 1013904223u;
        uint16_t oid = (r & 0x30000000) ? (uint16_t)(r >> 8) : 0xF1E0;
        uint16_t off = (uint16_t)((r >> 3) % 1600);
        size_t len = (r >> 13) % 300;
        int before_set = chip.set_ok;
        int err = optiga_cmd_set_data(l, oid, r & 1, off, data, len, &e);
        if (err != OPTIGA_E_PARAM)
            sent++;     /* reached the chip: accepted, or refused on bounds */
        CHECK(err == OPTIGA_OK || err == OPTIGA_E_PARAM || err == OPTIGA_E_CHIP,
              "random call %d: %d", i, err);
        if (oid != 0xF1E0)
            CHECK(err == OPTIGA_E_PARAM && chip.set_ok == before_set, "oid %04X went out", oid);
    }
    CHECK(sent > 100, "some random writes reached the chip (%d)", sent);
    check_container_only("random calls");

    printf("container: another program's data is not overwritten\n");
    open_chip(277);
    memcpy(chip.f1e0, "someone else's data", 19);
    chip.f1e0_len = 19;
    CHECK(optiga_container_read(l, back, sizeof back, &n, &st, &e) == OPTIGA_OK &&
          st == OPTIGA_CONTAINER_FOREIGN && n == 0, "reads as foreign");
    int sets = chip.set_ok;
    CHECK(optiga_container_write(l, data, 64, &e) == OPTIGA_E_FOREIGN, "write refused");
    CHECK(chip.set_ok == sets && chip.f1e0_len == 19 &&
          memcmp(chip.f1e0, "someone else's data", 19) == 0, "and nothing written");
    CHECK(strcmp(optiga_container_err_name(OPTIGA_E_FOREIGN),
                 "object holds other data; erase it first") == 0, "error text");
    CHECK(optiga_container_erase(l, &e) == OPTIGA_OK, "an explicit erase clears it");
    CHECK(optiga_container_write(l, data, 64, &e) == OPTIGA_OK, "then a write succeeds");

    printf("container: damaged blob is reported, then replaced\n");
    chip.f1e0[16 + 10] ^= 0x40;
    CHECK(optiga_container_read(l, back, sizeof back, &n, &st, &e) == OPTIGA_OK &&
          st == OPTIGA_CONTAINER_DAMAGED && n == 0, "CRC catches a flipped bit");
    fill(data, 64, 11);
    CHECK(optiga_container_write(l, data, 64, &e) == OPTIGA_OK, "overwrite a damaged blob");
    CHECK(optiga_container_read(l, back, sizeof back, &n, &st, &e) == OPTIGA_OK &&
          st == OPTIGA_CONTAINER_OK && n == 64 && memcmp(back, data, 64) == 0, "and read it");

    printf("container: power lost mid-write\n");
    /* Cut on each command of a 3-chunk write in turn (chunks 1-3, then the
       magic). The old blob is gone from the first command on, but no cut
       may ever leave something that reads as a valid blob. */
    for (int cut = 1; cut <= 4; cut++) {
        open_chip(64);
        fill(data, 300, 1);
        CHECK(optiga_container_write(l, data, 300, &e) == OPTIGA_OK, "old blob");
        fill(data, 600, 2);
        chip.cut_after_sets = cut;
        CHECK(optiga_container_write(l, data, 600, &e) != OPTIGA_OK, "cut %d reported", cut);
        chip.cut_after_sets = 0;
        chip.silent = false;
        optiga_cmd_wipe(l);
        /* After a power cut: a fresh link and application. */
        CHECK(optiga_link_init(l, &FAKE_BUS) == OPTIGA_OK, "re-init");
        CHECK(optiga_cmd_open_app(l, &e) == OPTIGA_OK, "re-open");
        CHECK(optiga_container_read(l, back, sizeof back, &n, &st, &e) == OPTIGA_OK, "read");
        if (cut == 1)
            CHECK(st == OPTIGA_CONTAINER_OK && n == 300, "cut before anything: old blob (%d)", st);
        else
            CHECK(st == OPTIGA_CONTAINER_INCOMPLETE && n == 0, "cut %d: incomplete (%d)", cut, st);
        CHECK(optiga_container_write(l, data, 600, &e) == OPTIGA_OK, "retry after cut %d", cut);
        CHECK(optiga_container_read(l, back, sizeof back, &n, &st, &e) == OPTIGA_OK &&
              st == OPTIGA_CONTAINER_OK && n == 600 && memcmp(back, data, 600) == 0,
              "retry reads back");
        check_container_only("power cut");
    }

    printf("container: a chip that refuses the write\n");
    open_chip(277);
    chip.f1e0_locked = true;
    CHECK(optiga_container_write(l, data, 64, &e) == OPTIGA_E_CHIP && e == 0x07,
          "refusal and its reason (0x%02X)", e);
    CHECK(optiga_container_erase(l, &e) == OPTIGA_E_CHIP && e == 0x07, "erase refused too");
    CHECK(link_wiped(l), "buffers wiped on the error path");
    CHECK(optiga_cmd_get_data(l, OPTIGA_OID_SEC, false, back, sizeof back, &n, &e) == OPTIGA_OK,
          "link still in step");

    printf("container: read-back catches a chip that stores the wrong bytes\n");
    open_chip(277);
    chip.bad_store = true;
    CHECK(optiga_container_write(l, data, 64, &e) == OPTIGA_E_VERIFY, "write not verified");
    CHECK(optiga_container_erase(l, &e) == OPTIGA_E_VERIFY, "erase not verified");
    CHECK(strcmp(optiga_container_err_name(OPTIGA_E_VERIFY), "read-back did not match") == 0,
          "error text");
    check_container_only("bad store");

    printf("container: metadata of 0xF1E0 is factory and untouched\n");
    open_chip(277);
    CHECK(optiga_container_write(l, data, 64, &e) == OPTIGA_OK, "write");
    CHECK(optiga_cmd_get_data(l, OPTIGA_OID_CONTAINER, true, back, sizeof back, &n, &e) == OPTIGA_OK &&
          n == 21 && back[2] == 0xC0 && back[4] == 0x01, "LcsO still creation");
    check_container_only("metadata");
}
#endif /* LEEK_SE_CONTAINER_WRITES */

int main(void)
{
    test_crc();
    test_frames();
    test_apdus();
    test_uid();
    test_cert();
    test_rng();
    test_link_happy(277);
    test_link_happy(64);
    test_link_happy(32);
    test_link_faults();
#if LEEK_SE_CONTAINER_WRITES
    test_container_apdus();
    test_container_format();
    test_container_round_trip(277);
    test_container_round_trip(64);
    test_container_round_trip(32);
    test_container_round_trip(16);
    test_container_refusals();
#endif

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
#if LEEK_SE_CONTAINER_WRITES
    printf("all OPTIGA checks passed, container build\n");
#else
    printf("all OPTIGA checks passed\n");
#endif
    return 0;
}
