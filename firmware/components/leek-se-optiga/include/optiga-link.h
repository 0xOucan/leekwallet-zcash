/**
 * OPTIGA Trust M — the data-link and network layers over an injected bus.
 *
 * Still no ESP-IDF here. The bus is three callbacks (one I2C write
 * transaction, one I2C read transaction, a delay), so the same code runs
 * against the real chip through leek-se.c and against a simulated chip in
 * sim/test_se_optiga.c. The simulation is written from the same reading of
 * the spec as this file, which means the host suite proves the two halves
 * agree with each other, not that either agrees with silicon. Only a board
 * with a chip on it can do that; the README lists what has and has not been
 * seen to work there.
 *
 * Not thread-safe: one optiga_link_t, one caller at a time. leek-se.c holds a
 * mutex around it.
 */

#ifndef OPTIGA_LINK_H
#define OPTIGA_LINK_H

#include <stddef.h>
#include <stdint.h>

#include "optiga-proto.h"

typedef struct {
    void *ctx;
    /** One complete write transaction to 0x30. 0 when every byte was ACKed;
     *  anything else is treated as a NACK and retried. */
    int (*write)(void *ctx, const uint8_t *buf, size_t n);
    /** One complete read transaction from 0x30. 0 on success. */
    int (*read)(void *ctx, uint8_t *buf, size_t n);
    void (*delay_ms)(void *ctx, uint32_t ms);
} optiga_bus_t;

enum {
    OPTIGA_OK = 0,
    OPTIGA_E_BUS,       /* NACKed through every retry: absent, or wedged */
    OPTIGA_E_TIMEOUT,   /* stayed busy past the deadline */
    OPTIGA_E_FRAME,     /* a frame failed its CRC or length check */
    OPTIGA_E_PROTOCOL,  /* well-formed, but not what the sequence allows */
    OPTIGA_E_SIZE,      /* response larger than the buffer offered for it */
    OPTIGA_E_PARAM,
    OPTIGA_E_CHIP,      /* the chip answered Sta=0xFF; see its error code */
    OPTIGA_E_REPLY,     /* response APDU malformed */
};

typedef struct {
    optiga_bus_t bus;
    uint16_t data_reg_len;  /* what the chip reported */
    uint16_t max_frame;     /* what we use: min(reported, OPTIGA_MAX_FRAME) */
    uint8_t frnr_out;       /* number of the last frame we sent */
    uint8_t frnr_in;        /* number of the last frame we received */
    size_t rx_len;          /* a received frame waiting in rx, or 0 */
    uint8_t tx[1 + OPTIGA_MAX_FRAME];
    uint8_t rx[OPTIGA_MAX_FRAME];
} optiga_link_t;

/** Read `n` bytes of a register (write the address, then read). */
int optiga_link_reg_read(optiga_link_t *l, uint8_t reg, uint8_t *out, size_t n);

/**
 * Bring the link to a known state. The board ties the chip's RST pin high,
 * so an ESP32 reboot does not reset the chip: a response from before the
 * reboot may still be waiting, and both sides' frame counters are stale.
 * This issues a warm reset through the SOFT_RESET register (volatile — the
 * same effect as pulsing RST, which nothing persistent survives or records),
 * drains anything left over, reads DATA_REG_LEN and resynchronises the frame
 * counters.
 */
int optiga_link_init(optiga_link_t *l, const optiga_bus_t *bus);

/**
 * Send one command APDU and collect its whole response, chaining in both
 * directions as the frame size requires. The response is the raw APDU
 * (Sta | UnDef | OutLen | OutData); optiga_apdu_response() takes it apart.
 */
int optiga_link_transceive(optiga_link_t *l, const uint8_t *cmd, size_t cmd_len,
                           uint8_t *resp, size_t cap, size_t *resp_len);

const char *optiga_link_err_name(int err);

#endif /* OPTIGA_LINK_H */
