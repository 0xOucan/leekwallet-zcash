/**
 * OPTIGA Trust M — the byte-level half of the protocol, and nothing else.
 *
 * Pure C with no ESP-IDF dependency, so the host suite can drive every line of
 * it (sim/test_se_optiga.c). Nothing in here touches a bus: it builds frames
 * and command APDUs into caller buffers, and checks and takes apart what came
 * back. optiga-link.c does the talking.
 *
 * Reference: Infineon "IFX I2C Protocol Specification" v2.03 (data-link,
 * network and register layout) and the "OPTIGA Trust M Solution Reference
 * Manual" (command APDUs, data objects, error codes). This is a reading of
 * those documents, not a port of Infineon's host library — see the component
 * README for why, and for which parts have only been checked on a host.
 *
 * Scope is deliberately narrow: the commands here are OpenApplication,
 * GetDataObject (data and metadata) and GetRandom. There is no builder for
 * SetDataObject, SetObjectProtected, key generation or anything else that
 * writes to the chip, and that absence is the point. A bring-up that cannot
 * express a write cannot perform one by mistake — including the lifecycle
 * state change that is permanent.
 */

#ifndef OPTIGA_PROTO_H
#define OPTIGA_PROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------ registers */

#define OPTIGA_I2C_ADDR         0x30    /* 7-bit, factory default */

#define OPTIGA_REG_DATA         0x80    /* frames in and out */
#define OPTIGA_REG_DATA_LEN     0x81    /* max frame size, 2 bytes, big-endian */
#define OPTIGA_REG_I2C_STATE    0x82    /* 4 bytes, see below */
#define OPTIGA_REG_SOFT_RESET   0x88    /* write FF FF: warm reset, volatile */

/* I2C_STATE byte 0. Bytes 2-3 are the length of the frame waiting in DATA. */
#define OPTIGA_STATE_BUSY       0x80
#define OPTIGA_STATE_RESP_RDY   0x40

/* --------------------------------------------------------- data link */

/*
 * A frame is FCTR | LEN(2) | DATA(LEN) | FCS(2). FCS is a CRC-16 over
 * FCTR, LEN and DATA, sent high byte first. A control frame has LEN = 0.
 *
 * FCTR: bit 7 frame type, bits 6-5 sequence control, bits 3-2 this frame's
 * number, bits 1-0 the number of the last frame received from the other side.
 * Frame numbers are two bits and wrap.
 */
#define OPTIGA_FCTR_CONTROL     0x80
#define OPTIGA_SEQ_ACK          0x00
#define OPTIGA_SEQ_NAK          0x20
#define OPTIGA_SEQ_RESYNC       0x40
#define OPTIGA_SEQ_MASK         0x60
#define OPTIGA_FRNR_SHIFT       2
#define OPTIGA_FRNR_MASK        0x0C
#define OPTIGA_ACKNR_MASK       0x03

#define OPTIGA_FRAME_OVERHEAD   5       /* FCTR + LEN + FCS */

/*
 * The network layer puts one control byte in front of each packet. Bit 3 says
 * the presentation layer (the shielded connection) is in use; this bring-up
 * never sets it. Bits 2-0 chain a long APDU across several frames.
 */
#define OPTIGA_PCTR_PRESENTATION 0x08
#define OPTIGA_PCTR_CHAIN_NONE   0x00
#define OPTIGA_PCTR_CHAIN_FIRST  0x01
#define OPTIGA_PCTR_CHAIN_MIDDLE 0x02
#define OPTIGA_PCTR_CHAIN_LAST   0x04
#define OPTIGA_PCTR_CHAIN_MASK   0x07

/*
 * Largest frame this code will build or accept. 0x0115 (277) is the size
 * Infineon's own host library negotiates; a chip that reports less in
 * DATA_REG_LEN is used at its own figure (optiga-link.c), never more.
 */
#define OPTIGA_MAX_FRAME        277

/** CRC-16 used for FCS: the CCITT polynomial, reflected (0x8408), seed 0,
 *  no final XOR — the variant catalogued as CRC-16/KERMIT. */
uint16_t optiga_crc16(const uint8_t *data, size_t len);

/** Build a data frame carrying `pctr` + `payload`. Returns the frame length,
 *  or 0 if it would not fit in `cap` or in OPTIGA_MAX_FRAME. */
size_t optiga_frame_data(uint8_t *out, size_t cap, uint8_t frnr, uint8_t acknr,
                         uint8_t pctr, const uint8_t *payload, size_t len);

/** Build a control frame (ACK, NAK or RESYNC). Always 5 bytes. */
size_t optiga_frame_control(uint8_t *out, uint8_t seq, uint8_t acknr);

typedef struct {
    bool control;           /* control frame (no packet) */
    uint8_t seq;            /* OPTIGA_SEQ_* */
    uint8_t frnr;
    uint8_t acknr;
    uint8_t pctr;           /* data frames only */
    const uint8_t *payload; /* data frames only: the packet after PCTR */
    size_t payload_len;
} optiga_frame_t;

enum {
    OPTIGA_FRAME_OK = 0,
    OPTIGA_FRAME_SHORT,     /* fewer than 5 bytes */
    OPTIGA_FRAME_LENGTH,    /* LEN disagrees with the bytes received */
    OPTIGA_FRAME_CRC,       /* FCS wrong */
    OPTIGA_FRAME_EMPTY,     /* data frame with no PCTR */
    OPTIGA_FRAME_CTRL_LEN,  /* control frame with a body */
};

/** Check and split a received frame. `f` points into `buf`; nothing is
 *  copied. Returns OPTIGA_FRAME_OK or the first fault found. */
int optiga_frame_parse(const uint8_t *buf, size_t len, optiga_frame_t *f);

/* ------------------------------------------------------- command APDUs */

/*
 * Command: Cmd | Param | InLen(2) | InData. Response: Sta | UnDef | OutLen(2)
 * | OutData, Sta 0x00 on success and 0xFF on failure, in which case the reason
 * is in data object 0xF1C2.
 *
 * Bit 7 of Cmd asks the chip to clear the last error code before it runs the
 * command. Every builder here sets it except the one that reads the error
 * code, which would otherwise erase what it came to read.
 */
#define OPTIGA_CMD_GET_DATA     0x01
#define OPTIGA_CMD_GET_RANDOM   0x0C
#define OPTIGA_CMD_OPEN_APP     0x70
#define OPTIGA_CMD_FLUSH_ERR    0x80

#define OPTIGA_APDU_MAX         24      /* the largest command built here */

/* Data objects this bring-up reads. All are readable "Always" from the
   factory; nothing on the list is a key or a secret. */
#define OPTIGA_OID_LCS_GLOBAL   0xE0C0  /* 1 byte */
#define OPTIGA_OID_SEC_STATUS   0xE0C1  /* 1 byte */
#define OPTIGA_OID_COPROC_UID   0xE0C2  /* 27 bytes */
#define OPTIGA_OID_SEC          0xE0C5  /* security event counter, 1 byte */
#define OPTIGA_OID_MAX_COM_BUF  0xE0C6  /* 2 bytes */
#define OPTIGA_OID_DEVICE_CERT  0xE0E0  /* Infineon device certificate */
#define OPTIGA_OID_LCS_APP      0xF1C0  /* 1 byte */
#define OPTIGA_OID_ERROR_CODE   0xF1C2  /* 1 byte, last command error */

/** OpenApplication with a fresh context (Param 0x00), naming the Trust M
 *  application by its AID. */
size_t optiga_apdu_open_app(uint8_t *out);

/** GetDataObject for the whole object, or its metadata. */
size_t optiga_apdu_get_data(uint8_t *out, uint16_t oid, bool metadata);

/** GetDataObject on 0xF1C2 without clearing it first. */
size_t optiga_apdu_get_error(uint8_t *out);

#define OPTIGA_RANDOM_MIN       8
#define OPTIGA_RANDOM_MAX       256

/** GetRandom from the TRNG (Param 0x00). Returns 0 outside 8..256. */
size_t optiga_apdu_get_random(uint8_t *out, uint16_t len);

enum {
    OPTIGA_APDU_OK = 0,
    OPTIGA_APDU_SHORT,      /* fewer than the 4 header bytes */
    OPTIGA_APDU_LENGTH,     /* OutLen disagrees with the bytes received */
    OPTIGA_APDU_FAILED,     /* Sta != 0x00: read 0xF1C2 for the reason */
};

/** Check a response APDU and point `data`/`len` at OutData. */
int optiga_apdu_response(const uint8_t *buf, size_t n,
                         const uint8_t **data, size_t *len);

/* ------------------------------------------------------- what came back */

/*
 * Coprocessor UID, data object 0xE0C2: 27 bytes, laid out as below. The
 * identifier bytes are read out and printed, not interpreted — a value table
 * from a datasheet would go stale as soon as Infineon ships a new revision,
 * and a wrong label is worse than a hex string.
 */
#define OPTIGA_UID_LEN          27
#define OPTIGA_CIM_INFINEON     0xCD

typedef struct {
    uint8_t cim;            /* chip identification manufacturer: 0xCD */
    uint8_t platform;
    uint8_t model;
    uint16_t rom_mask;
    uint8_t chip_type[6];
    uint8_t batch[6];
    uint16_t x, y;          /* die position on the wafer */
    uint32_t fw_id;
    uint16_t esw_build;     /* firmware build number, BCD */
} optiga_uid_t;

/** Split a coprocessor UID. False unless exactly 27 bytes. */
bool optiga_parse_uid(const uint8_t *buf, size_t len, optiga_uid_t *u);

/**
 * Find the X.509 certificate inside data object 0xE0E0. Infineon stores it
 * either bare (DER, first byte 0x30) or in a TLS-identity wrapper:
 *   0xC0 | len(2) | chain len(3) | cert len(3) | DER ...
 * The DER's own outer length is checked against what is there. Points
 * `der`/`der_len` at the first certificate; false if it is neither form.
 */
bool optiga_cert_find(const uint8_t *buf, size_t len,
                      const uint8_t **der, size_t *der_len);

/** Life-cycle state byte (0xE0C0, 0xF1C0) by its name in the manual. */
const char *optiga_lcs_name(uint8_t lcs);

/** Last-error byte (0xF1C2) by its name in the manual. */
const char *optiga_error_name(uint8_t code);

/* ---------------------------------------------------- TRNG sanity check */

/*
 * A smoke test, not a randomness test. It catches a dead source - stuck bits,
 * a constant, a repeat - and nothing subtler; no number of bytes this small
 * can certify a TRNG. The chip's random output is NEVER used as wallet entropy
 * by this component: seeds come from the ESP32's own source through the
 * entropy gate (src/entropy.c), and that is unchanged.
 */
typedef struct {
    size_t bits;
    size_t ones;
    size_t longest_run;     /* longest run of equal bits */
    size_t max_byte_count;  /* most occurrences of any one byte value */
    bool ok;
    const char *why;        /* first failed check, NULL when ok */
} optiga_rng_report_t;

/** Judge `len` bytes (at least 32) from GetRandom. */
bool optiga_rng_sanity(const uint8_t *buf, size_t len, optiga_rng_report_t *r);

#endif /* OPTIGA_PROTO_H */
