/* See optiga-link.h. */

#include "optiga-link.h"

#include <string.h>

/*
 * Timing, in polls of at least 1 ms each.
 *
 * The chip NACKs its address while it is busy, so a NACK is retried rather
 * than believed. Ten tries at 1 ms covers the guard time between
 * transactions with plenty to spare.
 *
 * A response can take a long time if the security event counter is high:
 * the manual describes a throttling delay of up to 5 s per protected command.
 * None of the commands here should raise that counter, but 6 s of polling
 * means a chip that is throttling still answers instead of looking dead.
 */
#define WRITE_TRIES         10
#define POLL_MAX            6000
#define NOT_BUSY_MAX        10      /* "idle, no response" reads tolerated */
#define FLUSH_MAX           16      /* stale frames drained at init */
#define SOFT_RESET_WAIT_MS  20      /* the chip's startup time, with margin */

static int bus_write(optiga_link_t *l, const uint8_t *buf, size_t n)
{
    for (int i = 0; i < WRITE_TRIES; i++) {
        if (l->bus.write(l->bus.ctx, buf, n) == 0) {
            /* Guard time before the next transaction. */
            l->bus.delay_ms(l->bus.ctx, 1);
            return OPTIGA_OK;
        }
        l->bus.delay_ms(l->bus.ctx, 1);
    }
    return OPTIGA_E_BUS;
}

static int bus_read(optiga_link_t *l, uint8_t *buf, size_t n)
{
    for (int i = 0; i < WRITE_TRIES; i++) {
        if (l->bus.read(l->bus.ctx, buf, n) == 0)
            return OPTIGA_OK;
        l->bus.delay_ms(l->bus.ctx, 1);
    }
    return OPTIGA_E_BUS;
}

int optiga_link_reg_read(optiga_link_t *l, uint8_t reg, uint8_t *out, size_t n)
{
    int err = bus_write(l, &reg, 1);
    return err ? err : bus_read(l, out, n);
}

/* Read the frame waiting in DATA into l->rx, whose length was given by
   I2C_STATE. */
static int fetch(optiga_link_t *l, size_t size)
{
    if (size > sizeof l->rx)
        return OPTIGA_E_SIZE;
    int err = optiga_link_reg_read(l, OPTIGA_REG_DATA, l->rx, size);
    if (err)
        return err;
    l->rx_len = size;
    return OPTIGA_OK;
}

/* Wait for a response frame and read it into l->rx. */
static int read_frame(optiga_link_t *l)
{
    int idle = 0;
    for (int poll = 0; poll < POLL_MAX; poll++) {
        uint8_t st[4];
        int err = optiga_link_reg_read(l, OPTIGA_REG_I2C_STATE, st, sizeof st);
        if (err)
            return err;
        if (st[0] & OPTIGA_STATE_RESP_RDY)
            return fetch(l, ((size_t)st[2] << 8) | st[3]);
        if (!(st[0] & OPTIGA_STATE_BUSY)) {
            /* Idle with nothing to say. Observed elsewhere to be transient
               just after a frame goes in, so a few are allowed. */
            if (++idle > NOT_BUSY_MAX)
                return OPTIGA_E_PROTOCOL;
        }
        l->bus.delay_ms(l->bus.ctx, 1);
    }
    return OPTIGA_E_TIMEOUT;
}

static int send_frame(optiga_link_t *l, size_t frame_len)
{
    l->tx[0] = OPTIGA_REG_DATA;
    return bus_write(l, l->tx, 1 + frame_len);
}

static int send_control(optiga_link_t *l, uint8_t seq, uint8_t acknr)
{
    size_t n = optiga_frame_control(&l->tx[1], seq, acknr);
    return send_frame(l, n);
}

int optiga_link_init(optiga_link_t *l, const optiga_bus_t *bus)
{
    if (!l || !bus || !bus->write || !bus->read || !bus->delay_ms)
        return OPTIGA_E_PARAM;
    memset(l, 0, sizeof *l);
    l->bus = *bus;

    /* Warm reset. A chip that does not take it NACKs; carry on regardless,
       since the drain and resync below recover the state on their own. */
    static const uint8_t reset[3] = { OPTIGA_REG_SOFT_RESET, 0xFF, 0xFF };
    if (bus_write(l, reset, sizeof reset) == OPTIGA_OK)
        l->bus.delay_ms(l->bus.ctx, SOFT_RESET_WAIT_MS);

    uint8_t len[2];
    int err = optiga_link_reg_read(l, OPTIGA_REG_DATA_LEN, len, sizeof len);
    if (err)
        return err;
    l->data_reg_len = (uint16_t)((len[0] << 8) | len[1]);
    /* The smallest frame that carries a useful packet is about 16 bytes; a
       figure below that is a different device, or a bus reading 0x00/0xFF. */
    if (l->data_reg_len < 16 || l->data_reg_len == 0xFFFF)
        return OPTIGA_E_PROTOCOL;
    l->max_frame = l->data_reg_len < OPTIGA_MAX_FRAME ? l->data_reg_len
                                                     : OPTIGA_MAX_FRAME;

    /* Drain anything from before. */
    for (int i = 0; i < FLUSH_MAX; i++) {
        uint8_t st[4];
        err = optiga_link_reg_read(l, OPTIGA_REG_I2C_STATE, st, sizeof st);
        if (err)
            return err;
        if (!(st[0] & OPTIGA_STATE_RESP_RDY)) {
            if (!(st[0] & OPTIGA_STATE_BUSY))
                break;
            l->bus.delay_ms(l->bus.ctx, 1);
            continue;
        }
        err = fetch(l, ((size_t)st[2] << 8) | st[3]);
        if (err)
            return err;
    }
    l->rx_len = 0;

    /* Both counters restart: our next frame is number 0, and the first frame
       we expect from the chip is number 0. 3 is "the one before 0". */
    l->frnr_out = 3;
    l->frnr_in = 3;
    return send_control(l, OPTIGA_SEQ_RESYNC, 0);
}

int optiga_link_transceive(optiga_link_t *l, const uint8_t *cmd, size_t cmd_len,
                           uint8_t *resp, size_t cap, size_t *resp_len)
{
    *resp_len = 0;
    if (!l || l->max_frame == 0 || !cmd || cmd_len == 0)
        return OPTIGA_E_PARAM;
    l->rx_len = 0;

    /* Each frame: 5 bytes of framing and one PCTR around the data. */
    size_t per_packet = (size_t)l->max_frame - OPTIGA_FRAME_OVERHEAD - 1;
    optiga_frame_t f;
    bool first = true;

    /* ---- out: the command, chained if it does not fit one frame */
    while (cmd_len > 0) {
        size_t n = cmd_len > per_packet ? per_packet : cmd_len;
        uint8_t chain;
        if (first && n == cmd_len)
            chain = OPTIGA_PCTR_CHAIN_NONE;
        else if (first)
            chain = OPTIGA_PCTR_CHAIN_FIRST;
        else if (n == cmd_len)
            chain = OPTIGA_PCTR_CHAIN_LAST;
        else
            chain = OPTIGA_PCTR_CHAIN_MIDDLE;
        first = false;

        l->frnr_out = (l->frnr_out + 1) & 3;
        size_t flen = optiga_frame_data(&l->tx[1], sizeof l->tx - 1, l->frnr_out,
                                        l->frnr_in, chain, cmd, n);
        if (flen == 0)
            return OPTIGA_E_SIZE;
        int err = send_frame(l, flen);
        if (err)
            return err;
        cmd += n;
        cmd_len -= n;

        err = read_frame(l);
        if (err)
            return err;
        if (optiga_frame_parse(l->rx, l->rx_len, &f) != OPTIGA_FRAME_OK)
            return OPTIGA_E_FRAME;
        if (!f.control) {
            /* The chip may skip the separate ACK and answer with data
               straight away. Only legitimate once it has the whole command. */
            if (cmd_len != 0)
                return OPTIGA_E_PROTOCOL;
            break;
        }
        l->rx_len = 0;
        if (f.seq != OPTIGA_SEQ_ACK || f.acknr != l->frnr_out)
            return OPTIGA_E_PROTOCOL;
    }

    /* ---- in: the response, possibly chained */
    size_t got = 0;
    bool more = true;
    first = true;
    while (more) {
        if (l->rx_len == 0) {
            int err = read_frame(l);
            if (err)
                return err;
            if (optiga_frame_parse(l->rx, l->rx_len, &f) != OPTIGA_FRAME_OK)
                return OPTIGA_E_FRAME;
        }
        l->rx_len = 0;
        uint8_t expect = (l->frnr_in + 1) & 3;
        if (f.control || f.frnr != expect || f.acknr != l->frnr_out)
            return OPTIGA_E_PROTOCOL;
        /* A protected (shielded) response would mean the chip believes a
           session is open that this code never opened. */
        if (f.pctr & OPTIGA_PCTR_PRESENTATION)
            return OPTIGA_E_PROTOCOL;
        l->frnr_in = expect;

        uint8_t chain = f.pctr & OPTIGA_PCTR_CHAIN_MASK;
        bool ok_chain = first ? (chain == OPTIGA_PCTR_CHAIN_NONE ||
                                 chain == OPTIGA_PCTR_CHAIN_FIRST)
                              : (chain == OPTIGA_PCTR_CHAIN_MIDDLE ||
                                 chain == OPTIGA_PCTR_CHAIN_LAST);
        if (!ok_chain)
            return OPTIGA_E_PROTOCOL;
        first = false;
        more = (chain == OPTIGA_PCTR_CHAIN_FIRST || chain == OPTIGA_PCTR_CHAIN_MIDDLE);

        /* Copy before acknowledging: the ACK is built in tx, the packet lives
           in rx, so the order is about the error path - an oversized
           response is still acknowledged, leaving the link in step. */
        bool fits = got + f.payload_len <= cap;
        if (fits)
            memcpy(resp + got, f.payload, f.payload_len);
        got += f.payload_len;

        int err = send_control(l, OPTIGA_SEQ_ACK, l->frnr_in);
        if (err)
            return err;
        if (!fits)
            return OPTIGA_E_SIZE;
    }
    *resp_len = got;
    return OPTIGA_OK;
}

const char *optiga_link_err_name(int err)
{
    switch (err) {
    case OPTIGA_OK:         return "ok";
    case OPTIGA_E_BUS:      return "no ACK on the bus";
    case OPTIGA_E_TIMEOUT:  return "timed out waiting for the chip";
    case OPTIGA_E_FRAME:    return "bad frame (CRC/length)";
    case OPTIGA_E_PROTOCOL: return "unexpected frame sequence";
    case OPTIGA_E_SIZE:     return "response too large";
    case OPTIGA_E_PARAM:    return "bad parameter";
    case OPTIGA_E_CHIP:     return "command refused by the chip";
    case OPTIGA_E_REPLY:    return "malformed response APDU";
    default:                return "unknown";
    }
}
