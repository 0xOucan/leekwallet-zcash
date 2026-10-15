/**
 * One task for all Zcash cryptography.
 *
 * The Rust signer peaks at about 50 KB of stack (measured under QEMU while
 * checking and signing a real PCZT), and account derivation runs Sinsemilla.
 * Neither fits the 10 KB protocol and BLE tasks or the 8 KB UI task, and
 * growing all three would cost far more RAM than one worker with a 64 KB stack.
 * Callers hand it a job and block until it is done; jobs run one at a time,
 * which also serialises every use of the Rust heap.
 *
 * On the host (LEEK_HOST_TEST) a job runs directly on the caller's stack.
 */
#ifndef LEEK_ZCASH_WORKER_H
#define LEEK_ZCASH_WORKER_H

#include <stdbool.h>

#define ZCASH_WORKER_STACK (64 * 1024)

typedef void (*ZcashJobFn)(void *ctx);

/** Start the worker. Idempotent. False if the task could not be created. */
bool zcash_worker_start(void);

/**
 * Run `fn(ctx)` on the worker and wait for it to finish.
 * False if the worker is not running or did not answer in time; `fn` has then
 * not run, or its result must not be trusted.
 */
bool zcash_worker_run(ZcashJobFn fn, void *ctx);

#endif /* LEEK_ZCASH_WORKER_H */
