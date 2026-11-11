/**
 * Test control surface for the fake SE container. See fake_se_store.c.
 */
#ifndef FAKE_SE_STORE_H
#define FAKE_SE_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "se-backup.h"

/** An empty, present chip; every fault off. */
void fake_se_reset(void);

/** The whole 1500-byte object image, writable. */
uint8_t *fake_se_image(void);

/** Replace the object with a valid LKSC container holding `payload`. */
void fake_se_put_container(const uint8_t *payload, size_t len);

void fake_se_present(bool present);
void fake_se_fail_io(bool fail);
void fake_se_verify_fails(bool fail);
void fake_se_corrupt_writes(bool corrupt);
int fake_se_writes(void);
int fake_se_erases(void);

#endif /* FAKE_SE_STORE_H */
