# Backing up a seed that holds real value

The device protects a seed while it is on the device. This document is about the
part the device cannot help with: the copy you write down, which is where most
funds are actually lost.

## The one rule

**The seed and the passphrase must never be in the same place.**

Everything below follows from that. If someone finds both, they have your funds.
If you lose both, so have you. Every decision here is about keeping those two
failure modes apart.

## Do not split a seed phrase by cutting it up

The tempting scheme is: write words 1-8 on paper A, 1-4 and 9-12 on paper B,
5-12 on paper C. Any two papers reconstruct all twelve. It looks like a 2-of-3
backup and it is not one.

**One paper alone is enough to be dangerous.** Someone holding words 1-8, who
knows their positions, does not face a 128-bit problem. They face four unknown
words — about 2^44 candidates before the checksum narrows it further. A rented
GPU rig grinds that in **days to weeks**, deriving addresses and watching for one
that has ever held a balance.

So a single lost or photographed paper turns "computationally impossible" into
"someone patient with a credit card". You would never know it had happened.

The property you wanted is called *information-theoretic security*: with a real
threshold scheme, one share reveals **nothing at all** — not "hard to reverse",
but zero. You cannot get that by cutting the plaintext into overlapping pieces,
in any arrangement. That is the entire reason Shamir's scheme looks complicated.

## "But I use a passphrase, so it does not matter"

Arithmetically it mostly holds. An attacker with eight words still needs the
other four *and* the passphrase, and cannot test either alone: a candidate is
only verifiable by deriving an address and checking the chain, which needs both.
Even a mediocre passphrase pushes the search out of reach.

It is still the wrong trade, for three reasons.

**It makes the passphrase your only real protection.** You began with 128 bits.
After the leak, everything rests on a passphrase that is realistically 30-40
bits, chosen by a human. You have quietly converted a strong secret into a weak
one, and nothing on the outside shows it.

**It usually is not stored as separately as people think.** A passphrase kept
near the papers, or guessable from what someone knows about you, contributes
nothing at the moment it matters.

**It adds a second thing that must survive.** Forget the passphrase and all
three papers together are worthless. You have not removed the backup problem,
you have created another one — while weakening the first.

A passphrase is protection against a **stolen complete backup**. Spending it to
justify a weakened backup spends the same protection twice.

## What to do instead

**Several complete copies of the seed, in different places, and the passphrase
somewhere else again.**

That gets both properties without any new cryptography:

- **One location is robbed.** The thief holds a complete, valid seed and still
  cannot spend: they do not have the passphrase. The full 128 bits are intact.
- **One location is lost, burned or flooded.** The others are complete. There is
  nothing to reconstruct and no threshold to satisfy.

Compare that with three partial papers, which are weaker against theft *and*
offer more ways to lose access. It is worse on both axes at once.

## Verify a restore before you trust it

A wrong passphrase does not fail. It produces a perfectly valid wallet that
simply is not yours — and if you send funds to it, they are gone.

So: record the **master fingerprint (XFP)** when you set the wallet up. It is
eight hex characters shown on the wallet screen and on both passphrase
confirmation screens, and it identifies the *seed*, not one address.

To check a restore, enter the seed, apply the passphrase, and confirm the XFP
matches what you recorded. If it does not, the passphrase is wrong — stop before
moving anything. This is the cheapest check available and it takes seconds.

## Practical notes

- **Paper is a serious option.** It has no firmware, no battery, no reader
  dependency, and is readable in fifty years. Metal is better against fire and
  water. Both beat anything that needs a device to interpret it.
- **Never photograph a seed.** A phone camera roll is the most common way a
  written backup becomes a lost one.
- **Test the restore before funding.** A backup you have never restored from is
  a hypothesis, not a backup.
- **Write down which wallet a backup is for** if you keep several — the XFP is
  the label that cannot be confused with another seed.

## The device's own copies: vault, SD card, secure element

Everything above is about the phrase on paper or metal. The device can also
keep encrypted copies of a seed, and on a development image with the OPTIGA
Trust M fitted (`esp32s3cam-color-se`) it keeps up to three:

| Copy | Where | Opened by | What it is for |
|---|---|---|---|
| **NVS vault** | the board's flash | the PIN (PBKDF2-HMAC-SHA512, AES-256-GCM; [VAULT.md](VAULT.md)) | **the master.** Every other copy is made from it or restored into it |
| **SD `.zkv` file** | `SEEDn.ZKV` on a microSD card | its own passphrase (8-64 characters) | a copy that leaves the board; opens on any LeekWallet or in `zecz` |
| **SE container** | OPTIGA object `0xF1E0` | the PIN it was made under | a second copy on the board, outside the flash |

The SE copy is **Level 1: the chip is left unlocked**, so the copy there is a
backup *location*, not extra security. Anyone with the board's I2C bus can read
it, overwrite it or erase it. It is sealed exactly as the vault seals a seed
(same KDF, same AES-256-GCM, with a salt of its own) and is exactly as strong as
the PIN: with a short PIN, a thief who reads the chip can search the PIN space
offline, as with a flash dump. No attempt counter protects it; that is Level 2
in [RESEARCH-SECURE-ELEMENT.md](RESEARCH-SECURE-ELEMENT.md), not built. None of
the three replaces the phrase on paper.

All three show the seed's **ZFP** (its ZIP 32 fingerprint, short form, with no
passphrase) so copies can be matched by eye. A BIP39 passphrase is never
stored in any of them: restoring a seed restores the passphrase-less wallet,
and the passphrase is typed again as always.

### Settings > Secure element

Turn the knob to choose, push to confirm, Cancel to go back. **Every action asks
for the PIN first**, checked as at unlock: a wrong PIN costs an attempt, and the
last attempt wipes the device, exactly as at unlock and Change PIN.

| Item | Does |
|---|---|
| Info | chip found or not, maker, lifecycle state (LCS), security event counter (SEC), and what the container holds: empty, a LeekWallet copy (with its ZFP if your PIN opens it), other data, or damaged. Never a secret. |
| Copy seed to SE | seals the open seed (stored or temporary) with its label, birthday and account under your PIN, writes it, reads it back and opens it before saying "Copied". If the SE already holds a *different* seed, or a copy your PIN does not open, it asks before replacing it. Data that is not a LeekWallet copy is never written over: erase it first. |
| Compare SE copy | opens the copy in RAM and says **Match** or **No match** against the open seed, with the copy's ZFP. Nothing else. |
| SE copy -> session | loads the copy as this session's temporary seed, as an SD load does: nothing reaches flash, and a lock or reboot forgets it. |
| SE copy -> vault | stores the copy in the vault under the rule below. |
| Erase SE copy | after "Erase the SE copy? The board vault and SD files are not touched", zeroes the container. |
| SD file -> SE | pick a `.zkv`, type its passphrase; the seed goes into the SE under your PIN (asking first if the SE holds a copy). The session's seed does not change. |
| SD file -> vault | pick a `.zkv`, type its passphrase; the seed goes into the vault under the rule below. |

If the vault's PIN does not open the SE copy (it was made under an earlier PIN,
or on another board), Compare and both restores offer to type the copy's own
PIN. That PIN is checked against the copy only, not against the vault's
attempt counter: the copy is readable off the bus anyway, so a counter here
would protect nothing.

Saving the open seed to a new `.zkv` and loading one for the session stay
where they were: the main menu's **SD Card** screen.

### The restore rule

Restoring into the vault (from the SE or from an SD file) **never overwrites or
deletes a seed that is already in the vault.** Before anything is written, every
vault slot is decrypted and compared with the seed being restored:

- **A slot already holds this seed:** nothing is written ("Already in the
  vault", with the slot number).
- **No slot holds it:** it goes into a new slot after the last one, and becomes
  the selected wallet (a passphrase typed for the previous seed is dropped, as
  on any wallet switch).
- **The vault is full (30 seeds):** refused, nothing written.
- **A slot does not decrypt:** refused, nothing written. The restore cannot tell
  whether that slot is this seed, and it does not guess.

There is no path in the code that writes over a vault slot from a backup; the
only write is the append (`wallet_store_mnemonic_if_new()` in
`components/leek-wallet`, `sdseed_restore_to_vault()` and
`sebk_restore_vault()` on top of it, `sim/test_se_backup.c` for the checks).

### If something breaks

- **The vault is lost or the board was wiped:** set a PIN, then Settings >
  Secure element > *SE copy -> vault* (type the old PIN when asked), or *SD
  file -> vault* with the card's passphrase. Check the ZFP against your notes.
- **The SE copy is damaged or erased:** *Copy seed to SE* again from the vault,
  or *SD file -> SE*.
- **The SD file is lost:** the main menu's SD Card > *Save open seed* makes a new
  one from the vault.
- **All three are gone:** the phrase on paper. That is why it comes first.

## If you genuinely want threshold backup

SLIP-39 (Shamir's Secret Sharing for mnemonics) is the standard, and it gives
the property naive splitting only imitates: any two of three shares recover the
secret, and any one reveals nothing. It is implemented in this tree
(`src/slip39-backup.c`, verified against all 45 official test vectors) and is
**not wired to any screen** — it costs zero bytes in the firmware today.

It is deliberately unfinished because most people do not need it: complete
copies plus a passphrase covers the same ground with fewer ways to go wrong. If
you want it, the remaining work is the interface, not the mathematics.
