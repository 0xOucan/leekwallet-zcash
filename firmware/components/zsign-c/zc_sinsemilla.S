/* The Sinsemilla S table (1024 points, x_le || y_le), embedded read-only from
 * the vendored sinsemilla_s.bin. zc_crypto_init() checks its BLAKE2b
 * fingerprint before any Sinsemilla operation. */
    .section .rodata
    .global zc_sinsemilla_s
    .type zc_sinsemilla_s, %object
    .balign 4
zc_sinsemilla_s:
    .incbin "sinsemilla_s.bin"
    .size zc_sinsemilla_s, . - zc_sinsemilla_s
#if defined(__linux__) && defined(__ELF__)
    .section .note.GNU-stack,"",%progbits
#endif
