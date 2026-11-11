/**
 * The secure element hook.
 *
 * ---------------------------------------------------------------------------
 * What this is
 *
 * The one question the rest of the firmware asks about a secure element, and
 * the only one it asks today: is there one? main.c calls se_probe() once at
 * boot, after the panel and before the UI, and logs the answer. Nothing
 * depends on it yet. What a fitted part is then used for - sealing the vault
 * key, attesting the device - is the driver's business and arrives with it
 * (docs/RESEARCH-SECURE-ELEMENT.md).
 *
 * Exactly one implementation is linked, chosen by LEEK_SE (board.h):
 *
 *   - LEEK_SE_NONE: se-none.c, which reports nothing fitted and touches no
 *     pin. Every build but the development image esp32s3cam-color-se.
 *   - LEEK_SE_OPTIGA: se-optiga.c, over components/leek-se-optiga.
 *   - anything else: the component that drives that part provides these two
 *     functions, and se-none.c compiles to nothing so the symbols cannot
 *     collide. It finds its bus in board.h (PIN_I2C_SDA / PIN_I2C_SCL).
 *
 * The hook is deliberately this small. A driver that needs more from the rest
 * of the firmware should add it here, with the stub's answer, so a build
 * without the part still links and still says why it has none.
 */

/* Not LEEK_SE_H: that is components/leek-se-optiga/include/leek-se.h, which
 * se-optiga.c includes beside this one. */
#ifndef LEEK_SE_HOOK_H
#define LEEK_SE_HOOK_H

typedef enum {
    SE_NOT_FITTED = 0,   /* this build drives no secure element */
    SE_ABSENT,           /* one was expected and did not answer */
    SE_PRESENT,          /* answered and identified itself */
    SE_FAULT,            /* answered and then misbehaved */
} se_status_t;

/** Look for the part, once. Safe to call with no part fitted. */
se_status_t se_probe(void);

/** A short name for the log: the part's, or "none". */
const char *se_name(void);

/**
 * The part's identity and state as short screen lines (at most
 * SE_LINE_LEN - 1 characters each): found or not, maker, lifecycle state,
 * security event counter. Never a secret, never the container's contents.
 * Returns the number of lines written; 0 with no part fitted. Blocks for a
 * few hundred milliseconds on a real chip, so the caller shows something
 * first.
 */
#define SE_LINE_LEN 22
int se_info_lines(char lines[][SE_LINE_LEN], int max);

#endif /* LEEK_SE_HOOK_H */
