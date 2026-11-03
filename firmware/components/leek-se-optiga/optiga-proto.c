/* See optiga-proto.h. Pure C: no bus, no allocation, no ESP-IDF. */

#include "optiga-proto.h"

#include <math.h>
#include <string.h>

/* ------------------------------------------------------------ CRC-16 */

/*
 * Bitwise rather than table-driven: it runs over at most a few hundred bytes
 * per frame, and eight shifts per byte is easier to check against the spec's
 * description than a 512-byte table nobody reads. The host suite pins it to
 * the catalogued check value (0x2189 over "123456789") and to avr-libc's
 * published _crc_ccitt_update(), which is the same function written as the
 * byte-at-a-time formula Infineon's own reference code uses.
 */
uint16_t optiga_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0x8408) : (uint16_t)(crc >> 1);
    }
    return crc;
}

static void put_crc(uint8_t *frame, size_t body)
{
    uint16_t crc = optiga_crc16(frame, body);
    frame[body] = (uint8_t)(crc >> 8);
    frame[body + 1] = (uint8_t)crc;
}

/* --------------------------------------------------------- data link */

size_t optiga_frame_data(uint8_t *out, size_t cap, uint8_t frnr, uint8_t acknr,
                         uint8_t pctr, const uint8_t *payload, size_t len)
{
    size_t total = OPTIGA_FRAME_OVERHEAD + 1 + len;
    if (total > cap || total > OPTIGA_MAX_FRAME)
        return 0;
    size_t body = 1 + len;      /* PCTR + payload */
    out[0] = (uint8_t)(((frnr << OPTIGA_FRNR_SHIFT) & OPTIGA_FRNR_MASK) |
                       (acknr & OPTIGA_ACKNR_MASK));
    out[1] = (uint8_t)(body >> 8);
    out[2] = (uint8_t)body;
    out[3] = pctr;
    if (len)
        memcpy(&out[4], payload, len);
    put_crc(out, 3 + body);
    return total;
}

size_t optiga_frame_control(uint8_t *out, uint8_t seq, uint8_t acknr)
{
    out[0] = (uint8_t)(OPTIGA_FCTR_CONTROL | (seq & OPTIGA_SEQ_MASK) |
                       (acknr & OPTIGA_ACKNR_MASK));
    out[1] = 0;
    out[2] = 0;
    put_crc(out, 3);
    return OPTIGA_FRAME_OVERHEAD;
}

int optiga_frame_parse(const uint8_t *buf, size_t len, optiga_frame_t *f)
{
    memset(f, 0, sizeof *f);
    if (len < OPTIGA_FRAME_OVERHEAD)
        return OPTIGA_FRAME_SHORT;
    size_t body = ((size_t)buf[1] << 8) | buf[2];
    if (body + OPTIGA_FRAME_OVERHEAD != len)
        return OPTIGA_FRAME_LENGTH;
    /* CRC before anything is believed, including the type bit. */
    uint16_t want = (uint16_t)((buf[len - 2] << 8) | buf[len - 1]);
    if (optiga_crc16(buf, len - 2) != want)
        return OPTIGA_FRAME_CRC;

    f->control = (buf[0] & OPTIGA_FCTR_CONTROL) != 0;
    f->seq = buf[0] & OPTIGA_SEQ_MASK;
    f->frnr = (buf[0] & OPTIGA_FRNR_MASK) >> OPTIGA_FRNR_SHIFT;
    f->acknr = buf[0] & OPTIGA_ACKNR_MASK;
    if (f->control)
        return body == 0 ? OPTIGA_FRAME_OK : OPTIGA_FRAME_CTRL_LEN;
    if (body == 0)
        return OPTIGA_FRAME_EMPTY;
    f->pctr = buf[3];
    f->payload = &buf[4];
    f->payload_len = body - 1;
    return OPTIGA_FRAME_OK;
}

/* ------------------------------------------------------- command APDUs */

/* The Trust M application identifier: D2 76 00 00 04 | "GenAuthAppl". */
static const uint8_t TRUST_M_AID[16] = {
    0xD2, 0x76, 0x00, 0x00, 0x04, 'G', 'e', 'n',
    'A',  'u',  't',  'h',  'A',  'p', 'p', 'l',
};

static size_t header(uint8_t *out, uint8_t cmd, uint8_t param, uint16_t in_len)
{
    out[0] = cmd;
    out[1] = param;
    out[2] = (uint8_t)(in_len >> 8);
    out[3] = (uint8_t)in_len;
    return 4;
}

size_t optiga_apdu_open_app(uint8_t *out)
{
    size_t n = header(out, OPTIGA_CMD_FLUSH_ERR | OPTIGA_CMD_OPEN_APP, 0x00,
                      sizeof TRUST_M_AID);
    memcpy(&out[n], TRUST_M_AID, sizeof TRUST_M_AID);
    return n + sizeof TRUST_M_AID;
}

size_t optiga_apdu_get_data(uint8_t *out, uint16_t oid, bool metadata)
{
    size_t n = header(out, OPTIGA_CMD_FLUSH_ERR | OPTIGA_CMD_GET_DATA,
                      metadata ? 0x01 : 0x00, 2);
    out[n++] = (uint8_t)(oid >> 8);
    out[n++] = (uint8_t)oid;
    return n;
}

size_t optiga_apdu_get_error(uint8_t *out)
{
    /* No flush bit: see the header. */
    size_t n = header(out, OPTIGA_CMD_GET_DATA, 0x00, 2);
    out[n++] = (uint8_t)(OPTIGA_OID_ERROR_CODE >> 8);
    out[n++] = (uint8_t)OPTIGA_OID_ERROR_CODE;
    return n;
}

size_t optiga_apdu_get_random(uint8_t *out, uint16_t len)
{
    if (len < OPTIGA_RANDOM_MIN || len > OPTIGA_RANDOM_MAX)
        return 0;
    size_t n = header(out, OPTIGA_CMD_FLUSH_ERR | OPTIGA_CMD_GET_RANDOM, 0x00, 2);
    out[n++] = (uint8_t)(len >> 8);
    out[n++] = (uint8_t)len;
    return n;
}

int optiga_apdu_response(const uint8_t *buf, size_t n,
                         const uint8_t **data, size_t *len)
{
    *data = NULL;
    *len = 0;
    if (n < 4)
        return OPTIGA_APDU_SHORT;
    if (buf[0] != 0x00)
        return OPTIGA_APDU_FAILED;
    size_t out_len = ((size_t)buf[2] << 8) | buf[3];
    if (out_len != n - 4)
        return OPTIGA_APDU_LENGTH;
    *data = &buf[4];
    *len = out_len;
    return OPTIGA_APDU_OK;
}

/* ------------------------------------------------------- what came back */

static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

bool optiga_parse_uid(const uint8_t *b, size_t len, optiga_uid_t *u)
{
    if (len != OPTIGA_UID_LEN)
        return false;
    u->cim = b[0];
    u->platform = b[1];
    u->model = b[2];
    u->rom_mask = be16(&b[3]);
    memcpy(u->chip_type, &b[5], 6);
    memcpy(u->batch, &b[11], 6);
    u->x = be16(&b[17]);
    u->y = be16(&b[19]);
    u->fw_id = ((uint32_t)b[21] << 24) | ((uint32_t)b[22] << 16) |
               ((uint32_t)b[23] << 8) | b[24];
    u->esw_build = be16(&b[25]);
    return true;
}

/* Outer length of a DER SEQUENCE at `p`, header included; 0 if malformed or
   longer than `avail`. Only the short form and the 1- and 2-byte long forms
   occur in a certificate this size. */
static size_t der_seq_len(const uint8_t *p, size_t avail)
{
    if (avail < 2 || p[0] != 0x30)
        return 0;
    size_t hdr, body;
    if (p[1] < 0x80) {
        hdr = 2; body = p[1];
    } else if (p[1] == 0x81 && avail >= 3) {
        hdr = 3; body = p[2];
    } else if (p[1] == 0x82 && avail >= 4) {
        hdr = 4; body = be16(&p[2]);
    } else {
        return 0;
    }
    return hdr + body <= avail ? hdr + body : 0;
}

static size_t be24(const uint8_t *p)
{
    return ((size_t)p[0] << 16) | ((size_t)p[1] << 8) | p[2];
}

bool optiga_cert_find(const uint8_t *buf, size_t len,
                      const uint8_t **der, size_t *der_len)
{
    *der = NULL;
    *der_len = 0;
    if (len == 0)
        return false;
    if (buf[0] == 0x30) {
        size_t n = der_seq_len(buf, len);
        if (n == 0)
            return false;
        *der = buf;
        *der_len = n;
        return true;
    }
    if (buf[0] == 0xC0 && len >= 9) {
        size_t outer = be16(&buf[1]);
        size_t chain = be24(&buf[3]);
        size_t cert = be24(&buf[6]);
        /* Each length must fit inside the one around it. */
        if (outer + 3 > len || chain + 3 > outer || cert + 3 > chain)
            return false;
        size_t n = der_seq_len(&buf[9], cert);
        if (n == 0 || n != cert)
            return false;
        *der = &buf[9];
        *der_len = n;
        return true;
    }
    return false;
}

const char *optiga_lcs_name(uint8_t lcs)
{
    switch (lcs) {
    case 0x01: return "creation";
    case 0x03: return "initialization";
    case 0x07: return "operational";
    case 0x0F: return "termination";
    default:   return "unknown";
    }
}

const char *optiga_error_name(uint8_t code)
{
    switch (code) {
    case 0x00: return "no error";
    case 0x01: return "invalid OID";
    case 0x02: return "invalid password";
    case 0x03: return "invalid param field";
    case 0x04: return "invalid length field";
    case 0x05: return "invalid parameter in data field";
    case 0x06: return "internal process error";
    case 0x07: return "access conditions not satisfied";
    case 0x08: return "data object boundary exceeded";
    case 0x09: return "metadata truncation error";
    case 0x0A: return "invalid command field";
    case 0x0B: return "command out of sequence";
    case 0x0C: return "command not available";
    case 0x0D: return "insufficient buffer/memory";
    case 0x0E: return "counter threshold limit exceeded";
    case 0x0F: return "invalid manifest";
    case 0x10: return "invalid/wrong payload version";
    case 0x2C: return "signature verification failure";
    case 0x2D: return "integrity validation failure";
    case 0x2E: return "decryption failure";
    default:   return "unknown error";
    }
}

/* ---------------------------------------------------- TRNG sanity check */

bool optiga_rng_sanity(const uint8_t *buf, size_t len, optiga_rng_report_t *r)
{
    memset(r, 0, sizeof *r);
    if (len < 32) {
        r->why = "fewer than 32 bytes";
        return false;
    }
    r->bits = len * 8;

    size_t counts[256] = {0};
    size_t run = 0;
    int prev = -1;
    for (size_t i = 0; i < len; i++) {
        counts[buf[i]]++;
        for (int b = 7; b >= 0; b--) {
            int bit = (buf[i] >> b) & 1;
            r->ones += (size_t)bit;
            run = (bit == prev) ? run + 1 : 1;
            prev = bit;
            if (run > r->longest_run)
                r->longest_run = run;
        }
    }
    for (int v = 0; v < 256; v++)
        if (counts[v] > r->max_byte_count)
            r->max_byte_count = counts[v];

    /*
     * Bounds wide enough that a working source essentially never trips them
     * (each is beyond five standard deviations or a 2^-15 tail at these
     * sizes) and narrow enough that a stuck or constant one always does.
     */
    double n = (double)r->bits;
    double dev = fabs((double)r->ones - n / 2.0);
    double run_limit = ceil(log2(n)) + 15.0;
    double lambda = (double)len / 256.0;
    double byte_limit = lambda + 6.0 * sqrt(lambda) + 8.0;

    if (dev > 5.0 * sqrt(n) / 2.0)
        r->why = "ones/zeros out of balance";
    else if ((double)r->longest_run > run_limit)
        r->why = "run of equal bits too long";
    else if ((double)r->max_byte_count > byte_limit)
        r->why = "one byte value repeats too often";
    r->ok = (r->why == NULL);
    return r->ok;
}
