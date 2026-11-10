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
>
> **Update 2026-11-10: research on how wallets use the Trust M, and a
> development-only SE container.** A summary of what Trezor and BitBox do
> with this chip is [below](#what-other-wallets-do-with-the-trust-m) (full
> brief: [RESEARCH-OPTIGA-WALLETS.md](RESEARCH-OPTIGA-WALLETS.md)), followed
> by the [three vault options](#vault-options-nvs-sd-card-and-se-container),
> the reversible Level 1 container, the hardened Level 2 design, and
> [open questions](#open-questions-for-the-se-container). Nothing has been
> written to any chip.

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
generation, no shielded-connection handshake, no arbitrary APDU). The one
exception is compiled only with `LEEK_SE_CONTAINER_WRITES=1`, which no env
sets: data writes to `0xF1E0`, the [Level 1 container](#level-1-reversible-development-only).
The default build, including the bench, does only this:

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

## What other wallets do with the Trust M

Summary of [RESEARCH-OPTIGA-WALLETS.md](RESEARCH-OPTIGA-WALLETS.md), which
has the object tables, the flows step by step and every source. Trezor's code
(GPL-3.0) was read for design and constants only.

- **Trezor Safe 3 and Safe 5** keep the seed encrypted on the MCU. The Trust
  M (V3) holds no seed; it gates a 32-byte `PIN_SECRET` (`0xF1D0`) that is
  released only after the chip has checked a digest of the stretched PIN
  (`STRETCHED_PIN`, `0xF1D4`, an AUTOREF object). Each check spends a
  monotonic counter (`0xE120`, limit 16); an HMAC stretching key (`0xF1D8`)
  has its own counter (`0xE122`), and a lifetime counter (`0xE121`, change
  Never, 600 000) caps every stretch the chip will ever do. The MCU mixes
  `PIN_SECRET` and the chip's outputs into the key-encryption key, so the bus
  only ever carries one-way digests, and a fully broken chip leaves the PIN
  as strong as MCU-only PBKDF2. Sources: Trezor's
  [`docs/core/misc/optiga.md`](https://docs.trezor.io/trezor-firmware/core/misc/optiga.html),
  `core/embed/sec/optiga/optiga.c`, `storage/storage.c` at `95a7289c`.
- **Trezor's PIN objects do not need the shielded connection.** The pairing
  secret (`0xE140`) protects only attestation and FIDO keys; if the handshake
  fails, the PIN still works. **BitBox02 Nova** makes the opposite choice and
  puts `Conf(E140)` on almost everything, which assumes its MCU keeps the
  pairing secret secret. Source: `bitbox02-firmware` `src/optiga/optiga.c` at
  `b27f5f68`.
- **Both lock their layouts**, and only in production builds: Trezor sets
  `LcsO = operational` under `PRODUCTION`, BitBox under `FINAL_LCSO_STATE`.
  Until an object's `LcsO` is operational, anyone on the I2C bus can rewrite
  its access rules (for example, make a secret readable), so an unlocked
  layout is a demonstration, not a protection. Locking is permanent.
- **Chip facts that shape the design** (Infineon SRM v3.70): 12 arbitrary
  data objects of 140 bytes (`0xF1D0`-`0xF1DB`) and two of 1500 bytes
  (`0xF1E0`, `0xF1E1`); four 8-byte counters (`0xE120`-`0xE123`), each good
  for 600 000 updates. **V1 parts lack** Auto, AUTOREF, HMAC and AES, so the
  pattern above needs **V3**; Express and MTR parts are V3 with `0xF1D0` and
  `0xE140` already locked to a cloud provisioning service. No Pallas,
  secp256k1 or BLAKE2 on the chip: signing stays on the ESP32 either way.
- **Genuineness** is the factory certificate in `0xE0E0` chained to
  Infineon's CA, plus a fresh challenge signed by its key in `0xE0F0`. That
  catches a counterfeit or emulated part. It does not stop a lab: EUCLEAK
  ([CVE-2024-45678](https://eprint.iacr.org/2024/1380)) describes a
  side-channel leak in Infineon's ECDSA, claimed to affect the Trust M.
- **The closest ESP32 design without a secure element** is Blockstream
  Jade's blind PIN server, and it relies on flash encryption and secure boot.

## Vault options: NVS, SD card and SE container

Three places the encrypted seed can live. The first two exist and **are not
changed by anything in this document**: no data is migrated, rewritten or
moved out of NVS or off a card. The third is new.

| | NVS vault | SD `.zkv` seed card | SE container |
|---|---|---|---|
| Where the ciphertext is | ESP32 flash, NVS namespace `colibri` | a file on a microSD card (`SEEDn.ZKV`) | OPTIGA data object `0xF1E0` |
| Key | PIN → PBKDF2-HMAC-SHA512 with a per-device salt, AES-256-GCM ([VAULT.md](VAULT.md)) | passphrase → PBKDF2, AES-256-GCM (`src/zkv.h`) | Level 1: the NVS vault's key, unchanged. Level 2: that key mixed with a secret the chip releases only on a correct PIN |
| Attempt limit | 3, in firmware; a reflash removes it | none: the card can be copied | Level 1: none beyond the NVS vault's. Level 2: a counter in the chip a reflash cannot reset |
| What a thief with the board gets | the ciphertext (flash dump) and an offline PIN search | the ciphertext, if they also have the card | Level 1: the same as NVS. Level 2: every guess must go through the chip |
| Capacity | 30 seeds | one seed per file | 1484 bytes: about five GCM mnemonic blobs |
| Status | shipped | shipped | Level 1 code and simulator tests, flag off, not wired. Level 2 design only |

### SE container vault slot

#### Level 1: reversible, development only

What exists on this branch, behind `LEEK_SE_CONTAINER_WRITES` (default 0):

- **One data object, default access rules.** `0xF1E0` keeps its factory
  metadata (read Always, change while `LcsO` < operational on V3 dumps). No
  metadata is written, `LcsO` stays in creation, and nothing is irreversible:
  erasing overwrites all 1500 bytes with zeros, after which the object reads
  as empty, and the next write puts it back. The code cannot build any other
  write (component README).
- **The ESP32 does the encryption**, exactly as for the NVS vault: the blob
  stored would be the same `nonce || ciphertext || tag` that
  `vault_encrypt()` produces under the PIN-derived key. The container treats
  it as opaque bytes and adds no crypto of its own.
- **Unlocked means storage only, with no added security.** Anyone with I2C
  access to the board can read `0xF1E0`, overwrite it, or rewrite its
  metadata, because nothing is locked. A thief who can dump the flash can as
  easily read the chip. Level 1 exists to bring up the write path, the
  format and the failure handling on real silicon while every step can still
  be undone; it must not be described to a user as a security feature.

**Why `0xF1E0`, 1500 bytes.** It is the larger of the Trust M's two object
sizes, so one object holds up to 1484 bytes of payload after the header:
about five vault blobs (a 24-word mnemonic is at most 256 bytes, so its GCM
blob is under 290). One object keeps the allowed write list to a single
entry. It sits outside the objects Trezor's and BitBox's PIN layouts use
(`0xF1D0`-`0xF1DB`, `0xE120`-`0xE123`, `0xE140`, `0xE200`), so Level 2 can
take the Trezor-compatible map without moving it, and it is not one of the
objects Express/MTR parts ship pre-locked. `0xF1E1` is left untouched as a
spare.

**Format** (`optiga-container.h`): a 16-byte header — magic `LKSC`, format
version 1, flags (0), payload length (u16), CRC-32 of the payload, 4
reserved zero bytes — then the payload. Reading classifies the object:

| State | Meaning | Write over it? |
|---|---|---|
| empty | never written, or all zeros (erased) | yes |
| ok | a valid container; the payload is returned | yes |
| incomplete | our header with the magic still zero: a write was cut | yes |
| damaged | our magic, but header or CRC wrong | yes |
| foreign | anything else, including a newer format version | **no**: erase it explicitly first |

The CRC catches a torn or decayed write; it is not authentication, since
anyone who can write the object can write a matching CRC. The GCM tag inside
the payload is what detects tampering.

**Write order.** The first command erases the object and writes the header
with the magic zeroed; the rest of the payload follows in 256-byte chunks at
increasing offsets; the 4-byte magic goes in last; then the whole object is
read back and compared. A power cut at any point leaves the old blob (cut
before the first command lands) or an *incomplete* one, never something that
reads as valid. The simulator cuts power on each command in turn to check
this. The previous blob is gone from the first command on, which is
acceptable only because Level 1 is a copy: the NVS vault stays the master.
Each write costs about seven chip commands; endurance (2 million programming
cycles per chip) is not a concern at development rates.

**Hygiene.** The container never logs its contents. The object image, the
command layer's response buffer and the link's frame buffers are wiped after
every read, write and erase, on error paths too. All of them are static, in
internal RAM, and under 2 KB; none is allocated, so none can land in PSRAM.

**Not wired yet.** Nothing in `src/` calls the container and `LEEK_HAS_SE`
is unchanged. The natural hook, for a development image built with the flag:
a "Secure element" screen under Settings (the info screen in the component
README) gains three entries, shown only in that build — *Copy vault to SE*,
*Compare SE copy* and *Erase SE copy*. Turn the knob to choose, push to
confirm, Cancel to back out; each asks for the PIN first. *Copy* writes the
current wallet's GCM blob exactly as NVS holds it; *Compare* reads it back
and reports match or mismatch, never the contents; *Erase* zeroes the
object. Restoring a wallet from the container is deliberately not part of
Level 1.

#### Level 2: hardened, future, spare chip first

The design that would make the chip matter. Nothing of it is written; every
step below waits for a spare V3 module that can be destroyed.

**Layout**, mirroring Trezor's storage v5 so the OIDs match a published,
reviewed table:

| OID | Role | Type | Read | Change | Execute |
|---|---|---|---|---|---|
| `0xF1D0` | `PIN_SECRET`, 32 random bytes | AUTOREF | Auto(`F1D4`) | Always | Always |
| `0xF1D4` | `STRETCHED_PIN`: digest of the stretched PIN | AUTOREF | Never | Auto(`F1D0`) | Luc(`E120`) |
| `0xF1D8` | `PIN_HMAC`: HMAC stretching key | PRESSEC | Never | Auto(`F1D4`) | Luc(`E122`) |
| `0xE120` | guess counter, limit N | UPCTR | Always | Auto(`F1D0`) | Always |
| `0xE122` | HMAC-use counter, limit N | UPCTR | Always | Auto(`F1D4`) | Always |
| `0xE121` | lifetime counter, 600 000 | UPCTR | Always | **Never** | Always |
| `0xE200` | optional AES-CMAC stretching key | AES | Never | Always | Luc(`E121`) |
| `0xF1E0` | the container: the vault blob | BSTR | Auto(`F1D4`) (option) | Auto(`F1D4`) | Always |

**Unlock.** The ESP32 runs PBKDF2 over the PIN with the device salt, as the
NVS vault does today. It sends the chip only `HMAC(stretched, "")`, never the
stretched value. A correct digest satisfies Auto(`F1D4`), spending one count
of `E120`; the chip then releases `PIN_SECRET`, and the HMAC step through
`F1D8` spends `E122`. The ESP32 computes
`KEK = HMAC(stretched, PIN_SECRET ‖ chip outputs)` and decrypts the vault
key with it. A sniffed bus sees digests and the released secret, which is
useless without the PIN; a chip that is fully broken leaves exactly today's
MCU-only PBKDF2 security, never less. Gating the container's read on
Auto(`F1D4`) as well would keep even the ciphertext off the bus without a
correct PIN; it costs nothing in the format.

**Counters.** `E120` and `E122` stop at N wrong attempts and can be reset
only by an Auto state that a correct PIN produces. `E121` (change Never)
bounds total guesses for the chip's life, across any reset. The firmware's
own counter is advanced *before* the check, and the two are kept in step, as
Trezor does; the firmware counter is a convenience a reflash can remove, the
chip's is the limit. N is an open question below.

**Why the PIN objects must not depend on the shielded connection.** This
board has no flash encryption. The pairing secret for `0xE140` would sit in
plain flash, so anyone with a flash dump can open a shielded session;
`Conf(E140)` would stop only a bus sniffer who has not dumped the flash.
Worse, by default `0xE140` is readable until it is locked. So the PIN
objects above have no `Conf(E140)` term, as in Trezor's layout; the
shielded connection is optional hardening for later (step 3 of
[What comes next](#what-comes-next)), useful mainly once flash encryption
or an eFuse-held HMAC key protects the pairing secret.

**V3 required.** Auto, AUTOREF, HMAC and AES do not exist on V1, and
Express/MTR parts arrive with `0xF1D0` and `0xE140` locked to someone else's
provisioning. The bench should tell V1 from V3 before any Level 2 work, with
reads only: the metadata of `0xE200` exists on V3 and not on V1 (to be
confirmed on the bench), and an Express part shows `0xF1D0` already
operational.

**Irreversible steps.** Each one is a metadata write, and Level 1 can
produce none of them:

1. Writing an object's `LcsO` (metadata tag `0xC0`) to **operational**
   (`0x07`): its access conditions (`D0`, `D1`, `D3`), key usage (`E1`) and
   data type (`E8`) are frozen for the life of the chip. Protected metadata
   update could change them later only if a trust anchor and reset type
   were provisioned for it, which this design never does.
2. Writing a **change** condition of **Never** (`D0 = FF`) and then locking:
   that object's data can never be written again. In this layout, `0xE121`.
3. A counter whose change is Never (`0xE121`) reaching its threshold: every
   object that spends it is unusable for good. (`E120`/`E122` at their limit
   are recoverable only with the correct PIN, which by then cannot be tried.)
4. Writing `0xE140` (pairing secret) with read and change Never and locking
   it: that module is tied to that secret for ever. Not part of Level 2 on
   this board.
5. `LcsO` to **termination** (`0x0F`): the object is unusable. Never used.

Everything before these — writing data, writing metadata while `LcsO` is
still creation, resetting counters — can be undone on an unlocked chip.

**Locking is a production decision with two keys.** A build flag (say
`LEEK_SE_PROVISION_LOCK`, default 0, never set in a development env) decides
whether the code to lock exists at all. Even then, locking runs only after
an explicit confirmation on the board: a screen that names the objects and
says "This cannot be undone", confirmed with the knob, Cancel to abort. The
metadata is read back and compared before and after writing, as Trezor's
`optiga_set_metadata()` does.

**No silent fallback.** A wallet created in OPTIGA mode records that mode in
its authenticated vault header. If the chip is missing or does not answer,
that wallet does not open, and the board says so: retry, or restore from the
seed backup onto a fresh vault. It never re-keys to MCU-only by itself. A
separate MCU-only mode (today's NVS vault) stays available for new wallets,
chosen explicitly.

**Genuineness check.** Before provisioning, and optionally at each boot:
verify the `0xE0E0` certificate chain to Infineon's CA, then have the chip
sign a fresh random challenge with `0xE0F0` and verify it against the
certificate's key. The signature is volatile on the chip (it spends no
counter, though private-key use raises the security event counter). It is a
counterfeit check, not lab-grade protection: a relaying chip on a modified
board passes it, and EUCLEAK suggests the key can be extracted with local
side-channel equipment.

### Open questions for the SE container

For the user to decide before any of this moves forward:

1. **Object choice.** `0xF1E0` alone (1484 bytes, about five wallets), or
   also `0xF1E1` for 30 wallets? A second object widens the allowed write
   list from one entry to two.
2. **What Level 1 stores.** A copy of the current wallet's NVS blob (the
   hook described above), or a separate slot with its own wallets?
3. **Blob format.** Is the 16-byte header (magic, version, length, CRC-32)
   enough, or should it carry the vault's salt or a device identifier so a
   container moved to another board is recognised as foreign?
4. **N for Level 2.** The firmware wipes after 3 today; Trezor allows 16,
   BitBox 10. A hardware limit is permanent for the secret it guards, so this
   wants deciding with the backup flow in view (as for the 608 above).
5. **Parts.** Is the bench module V3, V1 or Express? Buy two or three plain
   V3 parts (e.g. SLS 32AIA010MK) as spares before any metadata write.
6. **eFuse HMAC key for the pairing secret.** Burning one eFuse key block
   would let the S3's HMAC peripheral derive the pairing secret and a
   hardware salt without enabling flash encryption. Worth it, given that
   eFuse burns are permanent (BURN-PROCEDURE.md)?
7. **Release policy.** Proposed: the Level 1 flag never appears in a
   released image; Level 2 ships only with its own review and a tested
   brick-and-restore path.

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
- The 2026-11-10 brief, with commit hashes and every primary source for the
  sections above: [RESEARCH-OPTIGA-WALLETS.md](RESEARCH-OPTIGA-WALLETS.md)
  (Trezor `95a7289c`, BitBox02 `b27f5f68`, Infineon SRM v3.70, EUCLEAK,
  Blockstream Jade)
