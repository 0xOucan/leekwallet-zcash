# leek-se-optiga — OPTIGA Trust M bring-up

**Status: research and testing. Host-tested; not yet run against a chip.**
Nothing in this component stores a PIN, a key or a seed, and in the default
build nothing in it *can* write to the chip. It finds an Infineon OPTIGA Trust M on the board's
I2C bus, identifies it, reports its state and checks that the link and the
chip's TRNG work. The design it is meant to feed is in
[`docs/RESEARCH-SECURE-ELEMENT.md`](../../docs/RESEARCH-SECURE-ELEMENT.md).

## What it does, and what it will never do here

With `LEEK_SE_CONTAINER_WRITES` at 0 — the default, and every env in
`platformio.ini` except the development image `esp32s3cam-color-se`,
including `esp32s3cam-se-bench` — this table is complete:

| Operation | Command | Effect on the chip |
|---|---|---|
| Warm reset (at link init) | write `FF FF` to register `0x88` | volatile, same as pulsing RST |
| Resync frame counters | data-link control frame | volatile |
| `OpenApplication` (fresh context) | `F0 00 …` | volatile session context |
| `GetDataObject` data / metadata | `81 00/01 …`, `01 00 … F1C2` | none |
| `GetRandom` (TRNG) | `8C 00 …` | none |

That is the whole list. There is no builder for `SetDataObject`,
`SetObjectProtected`, key generation, `CalcSign`, or the shielded-connection
handshake, and no way to send an arbitrary APDU. `DATA_REG_LEN` is read, not
written. The host suite records every command code and register write the
simulated chip receives and fails if anything outside this table reaches it.

The random bytes from `GetRandom` are judged by a smoke test and discarded.
They never reach `entropy.c` or the wallet.

### With `LEEK_SE_CONTAINER_WRITES=1` (development only)

Exactly one more operation, on exactly one object:

| Operation | Command | Effect on the chip |
|---|---|---|
| `SetDataObject`, data only, on `0xF1E0` | `82 00 …` (write) or `82 40 …` (erase, then write) | **persistent**: rewrites the data of `0xF1E0`, nothing else |

This is the Level 1 SE container (`optiga-container.[ch]`, design in
[`docs/RESEARCH-SECURE-ELEMENT.md`](../../docs/RESEARCH-SECURE-ELEMENT.md#se-container-vault-slot)):
one blob of up to 1484 bytes in the 1500-byte arbitrary data object `0xF1E0`,
under a 16-byte header (magic `LKSC`, version, length, CRC-32). Its factory
access rules are left as they are, so it stays fully reversible: erase
overwrites all 1500 bytes with zeros.

What the flag still cannot produce: a write to any other object (the builder
returns nothing for any OID but `0xF1E0`, and the command layer returns
`OPTIGA_E_PARAM` without touching the bus), a metadata write (`Param 0x01`),
a counter update (`0x02`), a lifecycle change, key generation, a protected
update, the shielded connection, or an arbitrary APDU. The builder takes a
bool for erase-or-not, not a `Param` byte, so the others are not expressible.

The flag reaches the component through CMake, not `build_flags`:
`-DLEEK_SE_CONTAINER_WRITES=1` in an env's `board_build.cmake_extra_args`.
One env sets it: `esp32s3cam-color-se`, the development image whose
Settings > Secure element screen keeps a PIN-sealed copy of the seed here
(`src/se-backup.c`, `src/se-optiga.c`; docs/BACKUP.md). No release image does.
Level 1 is storage, not security — anyone with the
I2C bus can read the blob and rewrite the object's metadata — so only
already-encrypted data belongs in it.

## Files

| File | Depends on | Role |
|---|---|---|
| `optiga-proto.[ch]` | nothing | CRC, frame build/parse, command APDUs, UID/cert/LCS parsing, TRNG smoke test |
| `optiga-link.[ch]` | proto | IFX I2C data-link + network layer (ACKs, frame numbers, chaining, retries) over three injected callbacks |
| `optiga-cmd.[ch]` | link | the three read-only commands end to end, fetching `0xF1C2` on refusal; with the flag, the one `0xF1E0` data write |
| `optiga-container.[ch]` | cmd | flag only: the Level 1 container's format, write order, read-back and erase |
| `leek-se.[ch]` | ESP-IDF | legacy I2C driver as the bus, a mutex, SHA-256 of the certificate, the public API |

Everything above `leek-se.c` is pure C and is built on the host by
`sim/test_se_optiga.c` (`make -C sim test`, under ASan/UBSan), twice: as
`test_se_optiga` with the flag at 0, and as `test_se_container` with it at 1.

## Why hand-written, not Infineon's host library

Infineon's `optiga-trust-m` host library (MIT) is the obvious choice for a
product, and is the right one when this moves past read-only. For this
bring-up it was rejected, for reasons that are about this task, not the
library:

- **Audit surface.** The library is tens of thousands of lines: a full
  command set, the shielded connection, crypto PAL, OS abstraction, an event
  model with callbacks. This bring-up needs four commands. About 1,400 lines
  with comments, half of it the protocol, that a reviewer can read in one sitting, and that structurally cannot write
  to the chip, is easier to trust on a wallet's bus than a library whose
  every write path has to be shown unreachable.
- **Fit.** Its ESP-IDF PAL installs and owns the I2C driver. This board's bus
  is already owned by the panel (and ESP-IDF 5.5 refuses two I2C drivers in
  one image), so the PAL would need patching before the first byte moved.
- **Offline, pinned builds.** It was not available on this machine, and this
  build is reproducible by policy (see `platformio.ini`): vendoring would mean
  fetching an unpinned tree in the middle of a research task. If it is
  adopted later, pin an exact release tag as a git submodule and record its
  commit hash in `THIRD-PARTY-LICENSES.md`.
- **License** is not the blocker: MIT is compatible with this project's
  Apache-2.0. (Trezor's OPTIGA driver, the other well-tested open
  implementation, *is* a blocker — GPL-3.0 — and was used only to
  cross-check constants such as the AID and register map, not copied.)

The cost of the choice: the protocol reading is ours, so a misreading would
be ours too. Mitigations are in the test section below, and the real check is
the bench on hardware.

## Wiring (ESP32-S3 CAM board)

| Module pin | Board | Notes |
|---|---|---|
| VCC | 3V3 | the Trust M is 1.62–5.5 V; use 3V3, same rail as the bus |
| GND | GND | |
| SDA | GPIO47 | the existing I2C0 bus, shared with the panel at `0x3C` |
| SCL | GPIO21 | |
| RST | 3V3 | **must be tied high**: low or floating holds the chip in reset |

No address clash: the Trust M answers at `0x30`, the panel at `0x3C`. Most
breakouts carry their own I2C pull-ups; with the panel's as well the bus is
fine at 400 kHz. A 100 nF capacitor across VCC/GND at the module helps if the
breakout lacks one.

Because RST is tied high, an ESP32 reset does **not** reset the chip. The
link therefore starts every session with a warm reset through register
`0x88`, drains any response left over from before the reboot, and resyncs
the frame counters. A full cold reset is a power cycle.

## Running the bench

```
pio run -e esp32s3cam-se-bench -t upload
pio device monitor -e esp32s3cam-se-bench
```

This image has no wallet, no UI and no seed. It prints a report over USB
serial, then repeats it every 20 s. Expected on a healthy part (values vary
per chip; `…` stands for chip-specific bytes):

```
I (…) se-bench: ======== OPTIGA Trust M bring-up ========
I (…) se-bench: read-only: no keys, data objects, metadata or lifecycle state are written
I (…) se-bench:   0x30  OPTIGA Trust M (default address)
I (…) se-bench:   0x3C  OLED panel                    <- only if the panel is wired
I (…) se-bench: scan: 2 device(s)
I (…) se-bench: probe: ACK at 0x30
I (…) se-bench: link: DATA_REG_LEN …                   <- the chip's frame size
I (…) se-bench: UID CD……                               <- 27 bytes, starts with CD
I (…) se-bench:   maker      0xCD (Infineon)
I (…) se-bench:   firmware   id 0x……  build 0x……
I (…) se-bench: LCS global   0x07 operational
I (…) se-bench: LCS app      0x01 creation               <- or 0x07
I (…) se-bench: SEC          0 (security event counter; 0 on a fresh chip)
I (…) se-bench: cert 0xE0E0  object ~5xx B, X.509 DER ~5xx B
I (…) se-bench:   sha256 ……
I (…) se-bench:   meta E0F0 device private key        20…              <- TLV; expect read (D1) = never (FF)
I (…) se-bench: self-test: UID stable yes, TRNG ok (…/2048 ones, …), draws differ yes
I (…) se-bench: RESULT: PASS (… ms)
I (…) se-bench: screen preview:
I (…) se-bench:   |OPTIGA Trust M       |
```

What to do with the less happy outputs:

- **`nothing at 0x30`** — power, GND, SDA/SCL swapped, or RST not tied high.
  The scan line above it lists what *is* on the bus.
- **ACK at `0x30` but `open failed: no ACK` / `unexpected frame sequence`** —
  the chip is there but the link is not working. This is the case the host
  tests cannot rule out (see below); capture the log and, ideally, a logic
  analyser trace.
- **`maker … NOT the Infineon CIM 0xCD`** — treat the part as suspect.
- **`SEC` above 0** — something has caused security events on this chip
  before. Not harmful in itself; worth noting for a second-hand module.
- **`LCS global` not `operational`** — unusual for a shipped part; do not
  experiment further with it until understood.

Flash the ordinary `esp32s3cam` image afterwards.

## What has and has not been verified

**Host-verified** (`sim/test_se_optiga.c`):

- The FCS CRC against the catalogued CRC-16/KERMIT check value (`0x2189`) and
  against avr-libc's published `_crc_ccitt_update`, an independent formula.
- Control frames, data frames and every command APDU against bytes written
  out by hand; frame and APDU parsers against each fault in isolation.
- The `OpenApplication` AID against Infineon's published Trust M AID.
- The link layer against a simulated chip: chained responses at frame sizes
  277/64/32, chained commands at 16, NACK retries, BUSY polling, data with
  no separate ACK, a stale response surviving an ESP32 reboot, CRC
  corruption and recovery, silence, oversize responses, refusal and the
  `0xF1C2` read-back, an absent chip, implausible `DATA_REG_LEN`.
- A mutation check (CRC polynomial, initial frame numbers, idle limit, flush
  bit, stale-frame drain, response chaining) — each mutant fails the suite.
- Container build only: the `SetDataObject` bytes; that no OID but `0xF1E0`
  and no `Param` but `0x00`/`0x40` can be built (all 65,536 OIDs); write,
  read and erase round trips at frame sizes 277/64/32/16 (chained both ways);
  oversize and empty writes refused with nothing sent; a foreign object left
  alone; a damaged blob caught by the CRC; a power cut on each command of a
  write never leaving a blob that reads as valid; a chip that refuses the
  write; a chip that stores the wrong bytes (caught by the read-back); frame
  buffers wiped after every operation; 3000 random calls with
  nothing forbidden reaching the simulated chip.

**Only the board can verify**: that a real Trust M behaves as the simulation
does. The simulation was written by the same hand from the same spec, so the
suite proves the two agree, not that either matches silicon. In particular,
on first contact with hardware, watch for: the exact ACK/data ordering the
chip uses, the first frame's `ACKNR` after a resync, whether the warm reset
is accepted, and the I2C guard time at 400 kHz. For the container, also:
what `GetDataObject` returns for a never-written `0xF1E0` (the simulation
answers with zero bytes), and that erase-and-write leaves the object's
length at the bytes written (the simulation's reading of the manual).

## Integration (done, development image only)

`esp32s3cam-color-se` builds with `-DLEEK_SE=LEEK_SE_OPTIGA` (so
`LEEK_HAS_SE` is 1) and the container flag. `src/se-optiga.c` is the glue:
`se_probe()` (called once from `main.c` after the panel) installs the legacy
I2C driver on I2C0 when no I2C panel did (the colour kit; on an OLED kit the
panel's install is reused), calls `leek_se_attach(I2C_NUM_0)`, probes `0x30`
and opens the application; `se_info_lines()` feeds the Info screen; and the
four `sebk_store_*` functions put `leek_se_container_*` under
`src/se-backup.c`. Every call runs on the UI task with a "busy" screen up,
never on the draw path. Nothing is gated on the chip: without it the screen
says "No secure element" and the wallet works as before. With `LEEK_SE` left
at `LEEK_SE_NONE` (every other env) none of this is compiled.

`leek_se_last_err()` was added for that glue: the code behind the last
failure, so a refused write over foreign data, a failed read-back and an
oversize container can be told apart without parsing text.
