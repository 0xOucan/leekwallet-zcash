/* Platform glue the Rust crate links against (host simulator / tests). */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/random.h>

void *zs_platform_alloc(size_t size, size_t align)
{
    if (align < sizeof(void *)) align = sizeof(void *);
    void *p = NULL;
    return posix_memalign(&p, align, size ? size : 1) == 0 ? p : NULL;
}

void zs_platform_free(void *ptr)
{
    free(ptr);
}

void zs_platform_random(uint8_t *buf, size_t len)
{
    while (len) {
        ssize_t n = getrandom(buf, len, 0);
        if (n <= 0) abort();
        buf += n; len -= (size_t)n;
    }
}

void zs_platform_abort(const uint8_t *msg, size_t len)
{
    fprintf(stderr, "%.*s\n", (int)len, (const char *)msg);
    abort();
}

/* The prebuilt core for host targets references the unwinding personality
 * even with panic=abort; nothing unwinds, so an empty definition suffices. */
void rust_eh_personality(void) {}
