/**
 * The secure-element bench: an OPTIGA Trust M bring-up over USB serial.
 *
 * Compiled only under LEEK_SE_BENCH, which is an environment of its own
 * (`pio run -e esp32s3cam-se-bench`), the same way the QR bench is: it takes
 * the I2C bus for itself, never starts the wallet, and holds no seeds. It
 * scans the bus, identifies the chip, prints its state and runs the
 * read-only self-test, then repeats every 20 seconds so a monitor opened late
 * still sees a full report. See components/leek-se-optiga/README.md.
 */

#ifndef LEEK_SE_BENCH_H
#define LEEK_SE_BENCH_H

#ifndef LEEK_SE_BENCH
#  define LEEK_SE_BENCH 0
#endif

#if LEEK_SE_BENCH
/** Spawn the bench task. Call it and nothing else. */
void se_bench_start(void);
#endif

#endif /* LEEK_SE_BENCH_H */
