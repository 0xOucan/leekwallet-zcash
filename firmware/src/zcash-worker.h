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

/* 64 KB was for the Rust backend (~50 KB measured). The device runs the C
 * backend (zcash-sign.c), whose check and sign peak near 5 KB, and with
 * Bluetooth up a 64 KB internal-RAM stack no longer fits: the worker failed
 * to start on the first board and every derivation failed. 24 KB leaves the
 * C backend and trezor-crypto's BIP32 work wide margins. A build that runs
 * BOTH on the device (LEEK_ZCASH_BACKEND_BOTH) still gets 64 KB. */
#if defined(LEEK_ZCASH_BACKEND_BOTH) || defined(LEEK_HOST_TEST)
#define ZCASH_WORKER_STACK (64 * 1024)
#else
#define ZCASH_WORKER_STACK (24 * 1024)
#endif

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
