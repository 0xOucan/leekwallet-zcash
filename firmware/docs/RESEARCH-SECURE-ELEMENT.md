# ATECC608B as a PIN gatekeeper — research, not results

**Status: researched on paper only. No ATECC608B has been obtained, wired, or
talked to. Nothing in this document has been executed.** It exists so that
whoever does get hardware starts from the design questions already answered,
rather than from a datasheet.

> **Update 2026-11: an OPTIGA Trust M is on the bench.** The ATECC608B was
> never obtained; an Infineon OPTIGA Trust M module was. Its read-only
> bring-up is in `components/leek-se-optiga` and is described in
> [OPTIGA Trust M](#optiga-trust-m--read-only-bring-up) below. The 608 text
> is kept because the design questions are the same.

Companion to [BURN-PROCEDURE.md](BURN-PROCEDURE.md), which covers the on-chip
(eFuse / secure boot) route. The two solve overlapping problems; this one solves
strictly more of the physical-access problem, and costs a part.

---

## The problem neither software nor eFuses can solve

LeekWallet wipes the vault after 3 failed PINs. That counter lives in NVS, in
flash, on a chip whose firmware the attacker can replace. An attacker holding the
board reflashes it with a build whose counter never increments, and the 3-strike
limit is gone. A 4–8 digit PIN then falls in seconds.

The eFuse `HMAC_UP` binding does **not** fix this, and the reason is worth
restating because it is easy to get wrong: read-protection stops software
*reading* the key, not *using* it. After a reflash the software is the
attacker's, so they call the HMAC peripheral themselves. See NEXT-SESSION.md for
the full table — the short version is that eFuse binding defeats a **flash dump**
and does not defeat a **stolen board**.

The property that does fix it is: **the attempt counter must live in silicon the
main MCU cannot rewrite.** That is what a secure element is for, and it is why
Coldcard uses one this way.

## The model: gate, do not sign

The ATECC608B is **not** used to sign transactions. Signing stays in
the signers on the ESP32, where it is auditable and where the seed already
lives during an unlock.

The 608 holds one thing: a secret that is released **only** on a correct PIN, and
only a bounded number of times. The vault key becomes a function of both:

```
vault_key = KDF( PIN , secret_released_by_608 )
```

Get the PIN wrong N times and the 608 stops answering — permanently. The seed
is then unrecoverable by anyone, including the owner. That is the intended
behaviour, and it is why this must never ship without the backup flow being
tested first.

## Why the limit is a hardware guarantee

Three mechanisms, all documented in the ATECC608B datasheet:

- **Monotonic counters** — two of them, max 2,097,151, that only ever
  increment. There is no decrement command and no reset command.
- **`SlotConfig.LimitedUse`** — binds a slot's key to a counter, so using the
  key *is* incrementing the counter. They cannot be separated.
- **The `Lock` command** — permanently locks the config zone, the data zone,
  and individual slots. Once locked there is no unlock, in any command set, with
  any credential.

So the limit is not enforced by LeekWallet's firmware and cannot be relaxed by
replacing it. **Reflashing the ESP32 buys the attacker no additional attempts.**

## Wiring (SOIC-8)

| Pin | Signal |
|---|---|
| 4 | GND |
| 5 | SDA |
| 6 | SCL |
| 8 | VCC (3.3 V) |

Pins 1, 2, 3, 7 are no-connect. **Four functional wires.** Source: Microchip
DS40002239B (ATECC608B Summary Datasheet), Table 1 and the 8-lead SOIC pinout on
page 2.

> **Corrected 2026-09-16.** This table previously gave pin 5 as VCC, pin 7 as SCL
> and pin 8 as SDA, with pin 6 as no-connect. Wired that way, 3.3 V goes into the
> data pin and VCC is left floating. It was never checked against a datasheet
> because no chip had been obtained; it was checked the day one arrived.

A **100 nF ceramic capacitor** between pin 8 and pin 4, as close to the chip as
it will go, decouples the supply.

It shares the existing I²C bus — `SDA=GPIO8`, `SCL=GPIO9` — and answers at
`0x60` while the SSD1306 OLED is at `0x3C`, so there is no address conflict.
Pull-ups of 4.7 kΩ to 3.3 V are needed on SDA/SCL if the breakout does not
already carry them. A breakout board avoids fine-pitch soldering entirely.

## The mistake that would be permanent

**The I²C bus is in the clear.** A logic analyser on SDA/SCL captures every byte
crossing between the ESP32 and the 608 — including a secret released after a
correct PIN, if the naive design is used. Physical access is the exact threat
model this part is bought to address, so a plaintext bus gives most of it back.

This is not theoretical caution. [PQ1](https://github.com/EthereumPhone/PQ1)
treats an encrypted MCU↔SE channel as mandatory, using OPTIGA's AES-128-CCM-8
Shielded Connection and SE050's SCP03 — two secure elements, both with the
channel encrypted, no exceptions.

The 608's equivalent is the **I/O protection key** and encrypted read
(`Read`/`Write` in encrypted mode, with `CheckMac`/`GenDig` establishing the
session). The critical scheduling fact:

> **The slot configuration that enables encrypted I/O must be set before the
> config zone is locked, and the config zone lock is permanent.**

Retrofitting the encrypted channel later means starting with a fresh chip. So
the encrypted path has to be part of the first design, not a hardening pass.

## Suggested order of work, when hardware exists

Everything up to the final lock is **reversible**, which is what makes this a
cheap place to make mistakes:

1. **Buy several chips** (they are ~$1–2). Not one.
2. Wire a breakout on flying leads. Confirm the 608 answers at `0x60` alongside
   the OLED — an I²C scan is the whole test.
3. Develop the driver against an **unlocked** chip, which is fully rewritable.
4. Work out the complete slot configuration **including the encrypted I/O path
   and the `LimitedUse` counter binding**, and review it against the datasheet
   as a whole before committing.
5. Prove the failure behaviour: N wrong PINs, chip stops answering, vault
   unrecoverable — on a chip you are willing to destroy.
6. Prove the **backup and restore** flow survives it. A user whose 608 bricks
   must be able to restore from their seed phrase onto a new device.
7. Only then lock the config zone.

## Open questions

- **What is N?** Coldcard's model and PQ1's both sit around 10–13. LeekWallet's
  software limit is 3, which is aggressive for a hardware-enforced permanent
  brick. This wants deciding with the backup flow in view, not in isolation.
- **Does the 608 change the eFuse decision?** They are complementary — the
  608 stops the stolen-board attack, the eFuse binding stops the dump-only
  attack — but the eFuse burn is permanent and should not be spent on a scheme
  that a 608 would restructure. Design both, burn second.
- **Supply and authenticity.** Counterfeit and re-marked Microchip parts exist.
  Sourcing matters for a part whose entire value is that it behaves as
  specified.

## OPTIGA Trust M — read-only bring-up

**Status: code written and host-tested; not yet run against the chip.** The
module (pins VCC GND SCL SDA RST) is wired to the S3 CAM board; the bench
image `esp32s3cam-se-bench` is what to run first.

### Wiring

| Module | Board | |
|---|---|---|
| VCC | 3V3 | |
| GND | GND | |
| SDA | GPIO47 | existing I2C0, shared with the panel at `0x3C` |
| SCL | GPIO21 | |
| RST | 3V3 | tied high: no free GPIO. Low or floating = held in reset |

The Trust M answers at `0x30` (factory default). Tying RST high has one
consequence that shapes the driver: **an ESP32 reboot does not reset the
chip**, so a response from before the reboot can still be waiting and both
sides' frame counters are stale. The link opens every session with a warm
reset through register `0x88` (volatile, equivalent to pulsing RST), drains
leftovers and resyncs. A true cold reset needs a power cycle — or, later, a
GPIO on RST or a MOSFET on VCC, which is worth keeping in mind if a pin frees
up: Infineon's own host library uses both.

### What the bring-up does

Only volatile or read-only operations, and the component has no code path
for anything else (no `SetDataObject`, no metadata writes, no key
generation, no shielded-connection handshake, no arbitrary APDU):

- I2C scan; ACK probe at `0x30`.
- `OpenApplication` with a fresh context.
- Reads, all of objects readable "Always": coprocessor UID (`0xE0C2`, 27
  bytes; the first byte `0xCD` identifies Infineon), global and application
  lifecycle state (`0xE0C0`, `0xF1C0`), security event counter (`0xE0C5`),
  max comms buffer (`0xE0C6`), the factory certificate (`0xE0E0`, printed as
  its size and SHA-256), and the **metadata** (access rules) of a few
  objects, including the factory private key `0xE0F0` — its rules, not the
  key, which is not readable at all.
- Self-test: the UID read twice must agree; 256 bytes from `GetRandom` must
  pass a stuck-bit/constant/repeat smoke test, and two further draws must
  differ. The bytes are discarded; **the chip's TRNG is never wallet
  entropy.**

The protocol (IFX I2C v2.03 data-link and network layers, Trust M command
APDUs) is a minimal hand-written implementation rather than Infineon's host
library — the component README gives the reasons (audit surface, I2C-driver
ownership, offline pinned builds) and what is verified only on hardware.

### What comes next

In order, and each step stays reversible until the last:

1. **Run the bench.** Confirm identity, `LCS` operational, `SEC` 0, a sane
   certificate, and the self-test, on more than one module if possible.
2. **Verify the factory certificate** chain to Infineon's published root, on
   the device or the host. This is the answer to "is this a genuine Trust M"
   and it costs no writes.
3. **Shielded connection (MCU ↔ SE pairing).** The Trust M equivalent of the
   608's I/O protection key: a platform binding secret in `0xE140` shared
   with the MCU, then AES-128-CCM-8 on every APDU (Trezor's driver and PQ1
   both treat this as mandatory). Writing `0xE140` and **locking its
   metadata is the first irreversible step** in this whole plan, so it waits
   until the MCU side can keep that secret: with flash encryption and secure
   boot (BURN-PROCEDURE.md), or the secret is readable from a flash dump and
   the binding protects nothing.
4. **PIN attempt counter in silicon.** Trust M's model differs from the
   608's slot locks: a monotonic counter object (`0xE120..0xE123`, with a
   threshold) referenced by an access condition `Luc(counter)` on a secret,
   so every use of the secret increments the counter and the chip refuses at
   the threshold. Trezor Safe 3/5 run exactly this design on this chip; their
   [`docs/core/misc/optiga.md`](https://github.com/trezor/trezor-firmware/blob/main/docs/core/misc/optiga.md)
   documents the object layout and the stretching pattern and is the best
   reference available.
5. **Argon2id + SE.** The vault key becomes
   `KDF(Argon2id(PIN, salt), SE-stretch(...))`, where each SE step is
   `hmac(stretched, SE_op(hmac(stretched, "")))` as in Trezor's design:
   the chip only ever sees a one-way digest, so a sniffed bus or a malicious
   chip reduces security to the Argon2id step alone, never below it. The
   expensive memory-hard step stays on the ESP32, where it is auditable.
6. **Only then** decide the counter threshold (see the 608's open question
   on N), test the brick-and-restore path on a sacrificial module, and
   consider moving the application lifecycle state forward.

### Risks

- **Irreversible operations.** Lifecycle-state changes (`LcsO` → operational
  / termination) are one-way; metadata written with a `Never` change rule is
  permanent; a counter that reaches its threshold stays there; the
  platform binding secret, once locked, ties that module to that MCU
  forever. None of these are in this component. Anything that adds them
  wants its own review, and a stack of spare modules.
- **Supply chain.** These are cheap breakout modules from marketplaces, not
  parts from a distributor. A re-marked or emulating part could answer the
  same commands. The UID's `0xCD` byte is a weak signal; the certificate
  chain to Infineon's root (step 2) is the real one, and even that only
  proves the die, not the board around it.
- **Security event counter.** Failed authorisations raise `SEC`; above a
  threshold the chip throttles every protected command (up to ~5 s each).
  The bench reads it so a module with a history is noticed. Experiments that
  deliberately fail authorisation should expect it.
- **The bus is shared and in the clear** until step 3. Anything this
  bring-up reads is public anyway (identity, certificate); nothing secret
  may cross the bus before the shielded connection exists.
- **No hardware reset.** If the chip wedges in a way the warm reset does not
  clear, only a power cycle helps. Acceptable for a bench; for a product, a
  GPIO on RST is worth its pin.

## Sources

- Microchip ATECC608B datasheet (`SlotConfig`, `Lock`, monotonic counters,
  I/O protection key)
- PQ1, for the encrypted-MCU↔SE-channel requirement:
  <https://github.com/EthereumPhone/PQ1>
- Infineon, IFX I2C Protocol Specification v2.03, and the OPTIGA Trust M
  Solution Reference Manual (commands, data objects, error codes, lifecycle):
  <https://github.com/Infineon/optiga-trust-m>
- Trezor firmware's OPTIGA object layout and PIN design (reference only; the
  code is GPL-3.0 and was not copied):
  <https://github.com/trezor/trezor-firmware/blob/main/docs/core/misc/optiga.md>
