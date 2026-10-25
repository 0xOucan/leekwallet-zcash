/* The input hints (src/hints.h), built twice: as the reference board (four
 * buttons, the default here) and with -DLEEK_BOARD_S3CAM (knob + Cancel).
 *
 * Both: every hint fits the 21-column line. Four buttons: the original
 * strings, untouched. Knob: no hint names a key the kit does not have. */
#include <stdio.h>
#include <string.h>

#include "hints.h"

static int failures;
#define CHECK(c, ...) do { if (!(c)) { printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); failures++; } } while (0)

#define ROW_(h) { #h, h },
static const struct { const char *name, *text; } hints[] = { HINT_FOR_EACH(ROW_) };

/* Whole words only: "UP" inside "PUSH" would not be a key name. */
static int has_word(const char *s, const char *w)
{
    size_t n = strlen(w);
    for (const char *p = strstr(s, w); p; p = strstr(p + 1, w)) {
        int before = p == s || !(p[-1] >= 'A' && p[-1] <= 'Z');
        int after = !(p[n] >= 'A' && p[n] <= 'Z');
        if (before && after) return 1;
    }
    return 0;
}

int main(void)
{
    printf("== test_hints (%s)\n", LEEK_HAS_ENCODER ? "knob + Cancel" : "four buttons");
    for (size_t i = 0; i < sizeof hints / sizeof hints[0]; i++) {
        CHECK(strlen(hints[i].text) <= 21, "%s is %zu wide: \"%s\"",
              hints[i].name, strlen(hints[i].text), hints[i].text);
    }

#if LEEK_HAS_ENCODER
    static const char *const absent[] = { "UP", "DN", "DOWN", "BCK", "K1", "K2",
                                          "K3", "K4", "MIX MIX", "UP/DN", "^v", "+1", "key" };
    for (size_t i = 0; i < sizeof hints / sizeof hints[0]; i++) {
        for (size_t a = 0; a < sizeof absent / sizeof absent[0]; a++) {
            const char *w = absent[a];
            int hit = (w[0] >= 'A' && w[0] <= 'Z' && !strchr(w, ' ') && !strchr(w, '/'))
                          ? has_word(hints[i].text, w)
                          : strstr(hints[i].text, w) != NULL;
            CHECK(!hit, "%s names \"%s\": \"%s\"", hints[i].name, w, hints[i].text);
        }
        /* A footer that names an action for the Cancel button says so. */
        const char *t = hints[i].text;
        CHECK(!(has_word(t, "BACK") || has_word(t, "DEL") || has_word(t, "NO")) ||
                  strncmp(t, "X:", 2) == 0 || strncmp(t, "Keep", 4) == 0,
              "%s gives Cancel an action without X: \"%s\"", hints[i].name, t);
    }
    CHECK(strcmp(HINT_SCROLL_DEL_SEL, "X:DEL   TURN  OK:SEL") == 0, "PIN pad");
    CHECK(strcmp(HINT_REVIEW_NEXT, "X:REJECT TURN OK:NEXT") == 0, "review");
#else
    CHECK(strcmp(HINT_SCROLL_DEL_SEL, "UP DN  DEL  SEL") == 0, "PIN pad");
    CHECK(strcmp(HINT_WALLET_SELECT, "BCK  UP DN  SEL") == 0, "wallet list");
    CHECK(strcmp(HINT_CREATE_LENGTH, "UP/DN: 12/24") == 0, "12/24");
    CHECK(strcmp(HINT_REVIEW_NEXT, "REJECT   ^v      NEXT") == 0, "review");
    CHECK(strcmp(HINT_RECEIVE, "BACK    UP/DN    QR") == 0, "receive");
    CHECK(strcmp(HINT_PRESS_ANY, "Press any key") == 0, "boot");
#endif

    printf(failures ? "test_hints: %d FAILED\n" : "test_hints: ok\n", failures);
    return failures ? 1 : 0;
}
