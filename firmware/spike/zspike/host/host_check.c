/* Cross-architecture check for the Phase 0.4 spike.
 *
 * Recomputes FVK, address and cmx with the host build of zsign-rust and
 * compares them with what the Xtensa build printed under QEMU (or on a
 * board). Then verifies the device's RedPallas signature on the host, so a
 * miscompiled Xtensa binary cannot pass by agreeing with itself.
 *
 * usage: host_check <device-log>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zsign_rust.h"

static int find_hex(FILE *f, const char *key, uint8_t *out, size_t n)
{
    char line[1024], tag[64];
    snprintf(tag, sizeof tag, "ZSPIKE %s=", key);
    rewind(f);
    while (fgets(line, sizeof line, f)) {
        char *p = strstr(line, tag);
        if (!p) continue;
        p += strlen(tag);
        for (size_t i = 0; i < n; i++) {
            unsigned v;
            if (sscanf(p + 2 * i, "%2x", &v) != 1) return -1;
            out[i] = (uint8_t)v;
        }
        return 0;
    }
    return -1;
}

static int check(const char *what, const uint8_t *a, const uint8_t *b, size_t n)
{
    int ok = memcmp(a, b, n) == 0;
    printf("%-8s %s\n", what, ok ? "MATCH" : "MISMATCH");
    return ok ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc != 2) { fprintf(stderr, "usage: %s <device-log>\n", argv[0]); return 2; }
    FILE *f = fopen(argv[1], "r");
    if (!f) { perror(argv[1]); return 2; }

    uint8_t seed[64], alpha[32] = {0}, sighash[32];
    for (int i = 0; i < 64; i++) seed[i] = (uint8_t)i;
    for (int i = 0; i < 32; i++) sighash[i] = (uint8_t)(0xA0 + i);
    alpha[0] = 5;

    uint8_t dfvk[96], daddr[43], dcmx[32], dsig[64];
    if (find_hex(f, "fvk", dfvk, 96) || find_hex(f, "addr", daddr, 43) ||
        find_hex(f, "cmx", dcmx, 32) || find_hex(f, "sig", dsig, 64)) {
        fprintf(stderr, "device log is missing spike output\n");
        return 2;
    }

    uint8_t fvk[96], addr[43], cmx[32];
    int bad = 0;
    bad |= zs_spike_fvk(seed, 0, fvk) != 0;
    bad |= zs_spike_address(seed, 0, addr) != 0;
    bad |= zs_spike_cmx(seed, 100000, cmx) != 0;
    if (bad) { fprintf(stderr, "host computation failed\n"); return 2; }

    bad |= check("fvk", fvk, dfvk, 96);
    bad |= check("address", addr, daddr, 43);
    bad |= check("cmx", cmx, dcmx, 32);
    int v = zs_spike_verify(seed, 0, alpha, sighash, dsig);
    printf("%-8s %s\n", "sig", v == 0 ? "VALID (device signature verified on host)" : "INVALID");
    bad |= v != 0;

    /* Negative control: a flipped message must not verify. */
    sighash[0] ^= 1;
    int neg = zs_spike_verify(seed, 0, alpha, sighash, dsig);
    printf("%-8s %s\n", "negctl", neg == 1 ? "REJECTED (as expected)" : "ACCEPTED (BUG)");
    bad |= neg != 1;

    printf(bad ? "RESULT: FAIL\n" : "RESULT: PASS\n");
    return bad;
}
