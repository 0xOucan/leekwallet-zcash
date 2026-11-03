/**
 * The secure element stub: no part, no bus, no pins. See se.h.
 */

#include "se.h"
#include "board.h"

#if LEEK_SE == LEEK_SE_NONE

se_status_t se_probe(void) { return SE_NOT_FITTED; }

const char *se_name(void) { return "none"; }

#endif
