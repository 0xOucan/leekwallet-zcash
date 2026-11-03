# Design: remembered hosts

Status: **proposal, not implemented.** Nothing in the firmware or the hosts
changes until the owner decides on the questions in §8.

## 0. The rule this design starts from

The owner's decision, taken as fixed:

> **The six-digit code is compared on every connection, remembered host or
> not. There is no pairing shortcut, ever.**

Two reasons were given, and both shape everything below:

1. A malicious host or companion could abuse a stored pairing permission. A
   permission that lets a host connect without the user looking is a
   permission that malware on that host inherits.
2. The board will be used in hostile places: Bluetooth spoofing, relays
   (MITM), a look-alike host advertising or claiming a name the board
   remembers. Any shortcut in pairing is the thing those attacks go after.

So this document does **not** design "reconnect without the code". It
designs
what is still useful once the code always stays, and then asks whether that
is worth building (§6). The short answer is: a little, and probably not yet.

A second rule follows from the first and is used throughout:

> **Nothing remembered is ever authentication by itself.** A stored name, a
> pinned key or a "known device" label is only an extra warning signal shown
> on top of the code. Authentication is the code the user compares, every
> time.

## 1. What we have today

- **Handshake v2** (`PROTOCOL.md` §3, `src/session.c`): ephemeral X25519 on
  both sides, the board commits to its nonce before the host reveals its own,
  and the keys and the six digits are HKDF outputs over the shared secret,
  salted with `T = H(PKa ‖ PKb ‖ Na ‖ Nb)`. A relay gets one online guess at
  1 in 10⁶ (`AUDIT-TRANSPORT-2.md` §1-2).
- **Nothing is stored** on either side about the pairing
  (`host/leek-device/README.md`). No key outlives a connection.
- **The board has no long-term identity.** `PROTOCOL.md` §3: "none of this
  authenticates *which* device you are talking to. There is no long-term key
  and no attestation." The host learns which *wallet* it is talking to from
  `zGetAccount`'s seed fingerprint after pairing, and that is already pinned
  host-side: `leek-zcash/src/preflight.rs` refuses a PCZT for an account with
  another seed fingerprint, and the Chrome extension shows a stored account
  only while a board with the same fingerprint is connected
  (`docs/zcash/ROADMAP.md`, 2026-10-17). The app's `board-memory.ts` keeps the
  last link (USB, or the BLE id and advertised name) only to offer a one-tap
  reconnect; it is not authenticated and nothing treats it as such.
- **The viewing key** (`zGetAccount`) goes to any paired, unlocked session
  today with no press. A "Share viewing key with this device?" prompt is being
  added separately (asked once per session).
- **Flash is not encrypted** in the default build, there is no secure boot,
  and a logical NVS erase leaves the bytes in flash (`AUDIT-SECRETS-2.md`
  N2). The vault seeds are encrypted under a key derived from the PIN, which
  is only 4-8 digits.

## 2. What remembering can still do, with the code always compared

| Feature | What it gives the user | Needs |
|---|---|---|
| **A. "Known device" / "NEW device" on the pairing screen** | A warning cue: if you expected your usual laptop app and the board says NEW, something else is pairing (another app, another machine, a look-alike) | Board stores host static keys; handshake v3 proves the key |
| **B. Name-collision warning** | A host that calls itself "Laptop" but holds a different key gets a loud "NOT the remembered Laptop" | Same as A |
| **C. Skip the viewing-key prompt for a remembered host** (per host, default off) | One press less per session | A, plus stored grants |
| **D. Settings > Devices: list and forget** | The user sees which hosts the board remembers and can drop them | A |
| **E. The host pins the board** | The app warns when the board it talks to is not the one it used before | Already done, by seed fingerprint (§4.6, §5.2) |

What none of them do: let a host connect, unlock, sign, or read anything new
without the user in front of the board. The code, the PIN on the board, and
the review screens all stay exactly as they are.

## 3. Threat model

Attackers, and what remembering changes for each. "Today" is v2; "with
remembering" is the design in §4.

### 3.1 Passive radio listener (BLE)

- Today: learns nothing but the fact of a connection.
- With remembering: the host's static key travels **encrypted** (§4.2), so a
  listener cannot link two sessions to the same host by that key. The host's
  own BLE address and the board's advertised name are already linkable
  today and are unchanged.

### 3.2 Active relay / MITM, BLE spoofing, a look-alike host

- Today: caught by the code, at 1 in 10⁶ per attempt, if the user compares.
- With remembering: **still caught by the code, and only by the code.** In
  v3 the host's static key and its proof are inside the transcript the six
  digits come from (§4.2), so a relay cannot carry a "Known" label from one
  leg to the other without the digits differing. A look-alike host that
  sends a remembered *name* with its own key gets the collision warning (B).
  A host that sends a remembered *key* must also prove it holds the private
  key, or the handshake fails before anything is shown.
- What remembering must never do here: make a user more willing to skip the
  comparison because the screen says "Known". The pairing screen keeps the
  code as its largest element and the label stays secondary (§5.1).

### 3.3 Compromised or stolen remembered host

This is the cost. Precisely:

- An attacker who holds the remembered host's static private key (malware on
  that host, a copied key file, a stolen unlocked laptop) can make the board
  show **"Known: Laptop"** from any machine.
- It still has to get the user to compare and accept the code on the board,
  and unlock the board with the PIN on the board. It gets no signature
  without the review screens. With the code always required, a stolen key
  **cannot reconnect silently**.
- If C (viewing-key skip) is on for that host, it can read again, without a
  press, the viewing keys the board already gave that host (§4.5 scopes the
  skip to exactly those). It learns nothing it did not have, but it does learn
  them *again*, for example after the user deleted them on the host.
- So the real cost is a **false reassurance cue** ("Known") for an attacker
  who has the host's key, plus a re-read of already-shared viewing keys if C
  is on. How likely the key is to be stolen depends on where the host keeps it
  (§5.2): per-app OS keystores (Android Keystore, macOS Keychain with an app
  ACL, a Chrome extension's non-extractable WebCrypto key) are much better
  than Linux Secret Service, which any program of the same user can read.
- And the label's benefit is limited by the same fact: malware on the same
  machine *as* the legitimate app, with the same user, may be able to read the
  key and pass as "Known". The NEW cue is strongest against a different
  machine or a sandboxed different app.

### 3.4 Someone with the board's flash (lost or stolen board)

The flash can be read with `esptool` and is not encrypted.

- What they could read: the remembered-host list. In this design the list is
  encrypted under a key derived from the vault key (§4.8), so they need the
  PIN, by brute force offline, exactly as for the seeds. With the PIN they
  have the seeds too, and the host list is the least of it.
- If the list were stored in plain NVS instead, they would learn the host
  names ("Oucan's Pixel") and public keys: a **privacy** leak, not a key
  leak. Host public keys are not secret and let nobody impersonate a host.
- Can they **impersonate a host to the board?** No: the board stores host
  *public* keys only. The private keys never leave the hosts.
- Can they **impersonate the board to a host?** There is nothing to steal for
  that: the board has no static key in this design (§4.6). A host recognises
  the board by its seed fingerprint, which needs the seed. An attacker with
  the seed can already spend; impersonation adds nothing.
- Can they **add themselves** to the list by writing flash? Without secure
  boot, someone who can write flash can replace the firmware, and then the
  list is irrelevant. A MAC on the list (we get one from the AEAD) stops
  tampering that does not also know the PIN, which is all it can do. This is
  the same limit `VAULT.md` states for the seeds until flash encryption and
  secure boot are burned (T11).
- **Residue:** forgetting a host, or wiping the board, is a logical NVS erase
  and leaves ciphertext in flash (`AUDIT-SECRETS-2.md` N2). Encrypting the
  list is what makes that tolerable.

### 3.5 What the board cannot do

The board cannot stop a host from **storing** the viewing key, nor delete it
from the host. "Forget" on the board stops future reads only. A viewing key
reveals the whole history of the account, so host-side storage matters and
belongs in the SDK documentation (§5.4).

## 4. Protocol: handshake v3

### 4.1 Goals

1. Everything v2 has, unchanged: ephemeral keys, commit-then-reveal, the six
   digits over the whole transcript, one reveal per commitment.
2. The host may add a **static X25519 key** `S_h` and a short **label**,
   encrypted against passive listeners and **proven**: the host must show it
   holds the private key `s_h`.
3. The static key, the label and the version are **inside the transcript** the
   six digits come from (`PROTOCOL.md` §7 requires the version there as soon
   as two versions are accepted).
4. Identity is optional: a host that sends none is "Unnamed device", for
   example a one-shot CLI.
5. The board stays without a long-term key (§4.6).

### 4.2 Messages

```
host                                                      board
 ── hello { version: 3, hostPubkey: PKa } ───────────────────▶
 ◀── { version: 3, devicePubkey: PKb,
       deviceCommit: Cb = H("leek-session-commit-v3" ‖ PKb ‖ PKa ‖ Nb) }

   host: ee = X25519(a, PKb)
         k_id = HKDF(salt = H("leek-id-v3" ‖ 3 ‖ PKa ‖ PKb ‖ Cb ‖ Na),
                     ikm = ee, info = "leek-id-key-v3")
 ── helloReveal { hostNonce: Na,
                  hostId?: AEAD(k_id, nonce 0, S_h ‖ labelLen ‖ label) } ─▶
   board: ee = X25519(b, PKa); opens hostId if present (fail → refuse)
          se = X25519(b, S_h)            (only with hostId)
 ◀── { deviceNonce: Nb }

   host: checks Cb opens (as in v2); se = X25519(s_h, PKb)
   both: T3 = H("leek-session-transcript-v3" ‖ u8 3 ‖ PKa ‖ PKb ‖ Na ‖ Nb
                ‖ u8 hasId ‖ S_h (or 32 zero bytes) ‖ H(label))
         ikm = ee ‖ se   (ee alone without hostId)
         k_h2d, k_d2h, k_conf, passkey = HKDF(salt = T3, ikm, "…-v3" labels)
 ── helloConfirm { mac: HMAC(k_conf, "leek-host-confirm-v3") } ──▶
   board: checks mac (constant time). Fail → refuse, nothing painted.
          Looks S_h up in its list (§4.8) → Known / NEW / name collision.
 ◀── { ok: 1 }
 ◀── board shows the six digits (and the label, §5.1); user compares, ALLOW
 ── 0x11 encrypted traffic ──────────────────────────────────▶
```

Why each piece:

- **`S_h` goes with the reveal**, after the board has committed. A relay on
  the board-facing leg has fixed `S_h` (and its own `PKa`, `Na`) before `Nb`
  arrives, so it still cannot search; nothing the v2 analysis relies on moves.
- **`se` in the key material** means only a holder of `s_h` derives the same
  keys and code. A host that sends someone else's `S_h` cannot produce the
  `helloConfirm` MAC.
- **`helloConfirm` before painting** makes the label honest *before* the
  user reads it: the board shows "Known" only for a host that has proven the
  key. It costs one more round trip (three in total), which is milliseconds
  on USB and one connection interval or two on BLE.
- **Version, `hasId`, `S_h` and the label are in `T3`**, so a relay that
  strips the identity, swaps it, or changes the version produces different
  digits on the two screens. This also closes the gap `AUDIT-TRANSPORT-2.md`
  D-3 and `PROTOCOL.md` §7 recorded.
- **Forward secrecy stays.** `ee` is in every key, and `b` is ephemeral. A
  later theft of `s_h` does not open past sessions.
- **Rules carried over from v2:** at most one `helloReveal` per `hello`, at
  most one `helloConfirm` per reveal; the M-2 busy gate applies to
  `helloConfirm` too, since that is now the leg that paints; refuse an
  all-zero `ee` or `se` (small-order keys); host refuses `PKb == PKa`
  (R-5). Add the `AWAITING_REVEAL` timeout from R-7, and the same for the
  wait for `helloConfirm` (30 s).
- **Unknown CBOR keys** stay ignored and unhashed, as in v2. v3 reads
  nothing outside `T3`.

The label is printable ASCII, 1-16 bytes, refused (not truncated) otherwise,
the same rule `PROTOCOL.md` §3b uses for the BLE name.

### 4.3 What the six digits still protect

Everything. They are the only authentication of the channel: they prove the
board and the host derived the same `T3` and keys, so nobody is in the
middle and the identity the board just checked belongs to the endpoint the
user is looking at. The static key only answers "is this the same host
software as last time?", and only after the digits match.

### 4.4 Version negotiation and downgrade

- The board accepts `{2, 3}` for a transition period, then only 3. A v3 host
  offers 3; on `0x0002` from an old board ("speaks v2") it may retry with v2,
  and must then show "This board's firmware is older; it cannot recognise this
  app."
- A relay can force that downgrade by forging the plaintext `0x0002`, or by
  rewriting `version`. What it gains: the board shows no "Known" label. What
  it does not gain: v2 is secure on its own, and a relay running v3 to one
  side and v2 to the other is a full MITM, caught by the digits. **A
  downgrade can remove the warning cue but can never add a "Known" label**,
  because "Known" requires the v3 proof.
- When the board drops v2, the host's fallback goes too.

### 4.5 BLE and USB

The protocol is the same on both (the framing differs only as in
`PROTOCOL.md` §2). Notes:

- BLE is where the identity is most useful (a nearby attacker, a spoofed
  peer), and where encrypting `S_h` matters (passive listeners).
- We **do not** use OS-level BLE bonding (LESC "just works" / numeric
  comparison stored by the OS). It would be a pairing shortcut the owner
  ruled out, it binds to the operating system rather than the app (every app
  on the phone inherits it), and USB has no equivalent. Ledger's BLE transport
  relies on OS bonding (see `PeerRemovedPairingError` in the Ledger DMK);
  that is the design we are deliberately not copying.
- Remembering does not change "one transport at a time" (§3b). A host is
  remembered by its key, not by link, so the same app is "Known" over USB and
  BLE.

### 4.6 The board has no static key

We considered giving the board a long-term key so hosts can pin it. Under the
code-always rule it buys little and costs real things:

- It would sit in unencrypted flash: anyone who reads the flash can clone it,
  so a pin on it proves nothing against the attacker it is meant for.
- The host already pins what matters, the **seed fingerprint**. A different
  board with a different seed is detected today; a board with the same seed
  is in the hands of someone who can already spend.
- It would be a stable identifier for tracking, readable by any host that
  pairs.

Revisit only with flash encryption and secure boot (T11) or the secure element
(`RESEARCH-SECURE-ELEMENT.md`), where a key could be kept out of reach and
used for attestation.

### 4.7 Replay, rotation, expiry

- **Replay:** unchanged from v2. Fresh `PKb`, `Nb` and `PKa`, `Na` every
  time; there is no resumption and no cached key material. A captured
  `helloReveal` or `helloConfirm` fails under a new `T3`.
- **Rotation:** a host rotates by making a new key. The board sees a NEW
  device (or a name collision), the user compares the code as always, and
  chooses whether to replace the old entry (§5.1). No special protocol.
- **Expiry:** the board has no real-time clock, so no time limit. It keeps a
  pairing counter instead: each entry stores the counter value at its last
  use, and Settings shows "last used N pairings ago". An optional rule ("drop
  entries unused for 50 pairings") is a decision for §8.

### 4.8 Storage on the board

**Where:** NVS, namespace `leek_hosts`, one blob. Not the SD card: the list
describes the board's relationships, not a seed, and must work with no card.

**Format** (plaintext before encryption, version 1):

```
u8   format = 1
u32  pairing_counter
u8   count (≤ 8)
entry × count:
  32  S_h
  u8  label_len, 16 label (printable ASCII, zero-padded)
  u32 last_used (pairing_counter value)
  u8  flags: bit0 = skip viewing-key prompt
  u8  grant_count (≤ 4)
  grant × 4: u32 seed_fp_short, u16 account, u8 network
```

About 80 bytes an entry, under 700 bytes for 8.

**Encryption:** ChaCha20-Poly1305 (already in `session.c`), key
`HKDF(vault encryption key, "leek-hosts-v1")`, random 96-bit nonce per write,
stored `nonce ‖ ciphertext ‖ tag`. Authenticated, unlike the vault's current
AES-CBC. Consequences:

- The list can be read only while the board is **unlocked**. When a pairing
  happens on a locked board, the screen says "Unlock to check this device"
  instead of Known or NEW, and the check runs again after the PIN. In
  practice the board asks for the PIN at power-on, so this is rare.
- **Assumption to verify (step 0 of §7):** that the vault encryption key
  exists once a PIN is set, even with no stored seed (stateless and SD use).
  If it does not, derive the hosts key from the same PIN KDF with a new label
  at unlock.
- A PIN change re-encrypts the list with the new key, in the same place
  `wallet` re-encrypts the seeds.

**Limits:** 8 hosts. When full, the "Remember?" screen says the list is full
and offers "Replace oldest" or "Not now". Nothing is evicted without the user
choosing it.

**Wipe:** the three-wrong-PIN wipe and Wipe Device erase `leek_hosts`. It goes
into `device_wipe()` with the resume marker, so an interrupted wipe finishes it
too. (Physical sector erase, `AUDIT-SECRETS-2.md` N2, applies here as to
everything else; the list is ciphertext under a key that no longer exists, but
the salt residue means a PIN brute force still opens it, exactly as for the
seeds.)

**Stateless, temporary and SD seeds:** the host *entry* (key, label) is about
the host, not the seed, so it can be stored while one of these seeds is open.
But **grants** (§4.9) for these seeds are kept in RAM only and dropped on
lock, the same rule `zcash-meta.c` uses for birthdays: flash must never show
that such a seed was used, or with which host. Passphrase wallets likewise.
And a device-level setting **Remember devices: Off** stores nothing at all
(the board behaves exactly like v2, with "Unnamed device" for everyone); an
owner who wants the board to keep no trace of the hosts it met sets it Off.

### 4.9 The viewing-key prompt and grants (feature C)

This is the only place a remembered host gets anything a new host does not,
so it is the part to weigh most carefully against the rule in §0. It is
**not** an authentication shortcut: the code was compared in this session
before any request is served. It is a stored *consent*. Its limits:

- Off by default for every host. Turned on per host by the user on the board,
  never by the host.
- **Scoped to what was already shared:** a grant is `(seed fingerprint short,
  account, network)`, recorded when the user said yes on the prompt. The skip
  applies only to an exact match. A new account, another seed, the other
  network, a passphrase wallet: the prompt always appears. So a remembered
  host that turns hostile can only re-read keys it already received.
- **Visible:** when the skip is used the board shows "Viewing key sent to
  Laptop" for two seconds, so a read never happens unseen.
- Stored grants only for stored seeds without a passphrase; RAM-only
  otherwise (§4.8).

If the owner judges that even this is a "stored permission a malicious host
could abuse", drop C and keep the per-session prompt; A, B and D stand on
their own.

## 5. UI and hosts

### 5.1 Board screens

Knob plus one Cancel (K3) only. Footers use the fixed vocabulary in
`src/hints.h` (`X:<word>  TURN  OK:<word>`), 21 columns, checked by
`sim/test_hints.c`. Layouts are the 21-column, 8-line text grid every current
screen uses; the colour panel draws the same lines and adds colour (green
Known, amber NEW, red collision).

**Pairing, known host** (the code stays the largest, central element):

```
      Connect?
   Known: Laptop
     ID 1A2B 3C4D
      123 456
   Match the code
  shown in the app
X:DENY      OK:ALLOW
```

**Pairing, new host** (line 3 reads "Unnamed device" for a host with no
identity, "Unlock to check" on a locked board):

```
      Connect?
    NEW device:
  "Laptop" 9F8E 7D6C
      123 456
   Match the code
  shown in the app
X:DENY      OK:ALLOW
```

**Pairing, name collision** (same label as a remembered host, other key):

```
  ! NOT your Laptop !
 Same name, new key
   ID 9F8E 7D6C
      123 456
 Deny unless you just
 reinstalled the app
X:DENY      OK:ALLOW
```

The **ID** is the first 4 bytes of `SHA-256("leek-host-id-v1" ‖ S_h)` in hex.
The host app shows the same ID in its own settings, so a user can check it
once. The board never trusts the label alone: it is always printed beside
the ID, in quotes on NEW, and a collision is flagged.

**Remember this device?** Shown once, after ALLOW, only for a NEW host that
sent an identity, when Remember devices is On:

```
Remember this device?
   "Laptop"
   ID 9F8E 7D6C
 You will still match
 the code every time.
X:NOT NOW    OK:SAVE
```

On a collision the same screen reads "Replace the old Laptop?". When the list
is full, "List full: replace oldest?".

**Viewing-key prompt.** The separate per-session prompt, with one addition for
a remembered host: a choice turned with the knob.

```
 Share viewing key?
 Laptop  acct 0  main
 > Share this time
   Share, always here
X:NO    TURN  OK:SEL
```

"Always here" sets the skip flag and records the grant (§4.9). A NEW host gets
only "Share this time", or X:NO.

**Settings > Devices:**

```
 Devices        3/8         Laptop
 > Laptop                   ID 1A2B 3C4D
   Pixel                    Used 2 pairings ago
   Chrome                   View key: ask
   Remember: On             > Ask / Always
   Forget all               > Forget
X:BACK  TURN  OK:SEL        X:BACK  TURN  OK:SEL
```

"Forget" and "Forget all" ask once more ("Forget Laptop? It keeps any viewing
key it already has."). "Remember: Off" also asks whether to forget the
existing list. Nothing here is reachable over the protocol except "forget
me" (§5.2).

### 5.2 Host side (`leek-device`, `leek-zcash`, SDK)

- `HostIdentity { secret: [u8; 32], label: String }` and a
  `HostKeyStore` trait (load, create, delete). `Device::pair(show)` stays and
  sends no identity; `Device::pair_as(show, &HostIdentity)` sends one.
  `PairInfo` returns the negotiated version and the host ID to display.
- Key storage, per platform: desktop through the `keyring` crate (macOS
  Keychain, Windows Credential Manager, Linux Secret Service, with the note
  that Secret Service gives no per-app isolation); Android through a
  Keystore-held AES key wrapping the X25519 secret (leek-android passes the
  blob in over JNI; Zodl fork the same); Chrome extension and the app's
  webview core (`app/packages/core`) through a non-extractable WebCrypto
  X25519 `CryptoKey` in IndexedDB, falling back to a Keystore/keyring-wrapped
  key in the Tauri shell. A host that cannot store a key safely sends none.
- New encrypted method `forgetMe {}`: removes the caller's own entry (only
  the key that proved itself in this session). Lets an app's "Unpair" clean
  the board without a trip to Settings. It can only shrink what the board
  keeps.
- `zecz` (CLI) sends no identity unless asked (`--remember-as NAME`).
- **Board pinning stays by seed fingerprint**, already done in
  `preflight.rs` and the extension. Add the same check where it is missing
  (leek-android before reusing a stored account; the Zodl fork's import).
- `board-memory.ts` is unchanged: a reconnect convenience, never trust.

### 5.3 Migration from v2

1. Hosts learn v3 with v2 fallback (§4.4) and ship first.
2. Firmware accepts `{2, 3}`; v2 hosts keep working as "Unnamed device".
3. A later firmware accepts only v3; hosts drop the fallback.

No stored state exists in v2 on either side, so nothing migrates.

### 5.4 SDK guidance (honest limits)

To go in the SDK docs, since the board cannot enforce it:

- Store the UFVK and wallet database encrypted at rest (DB key in the OS
  keystore; SQLCipher or equivalent), and delete them on "Remove wallet".
- Never log the UFVK; never sync it to cloud backup.
- "Forget" on the board does not remove anything from the host; say so in the
  app.
- Show the host ID in the app's settings so users can compare it with
  Settings > Devices.

## 6. Alternatives and recommendation

| Option | Firmware | Hosts | User gets | Cost |
|---|---|---|---|---|
| **1. Nothing stored (today) + per-session viewing-key prompt** | prompt only (in progress) | none | one code + one prompt per session | none |
| **2. Option 1 + host-side board pinning by seed fingerprint everywhere** | none | small | a warning when the board's wallet is not the expected one | small |
| **3. Full remembered hosts (A+B+C+D, this design)** | v3 handshake, encrypted list, 5 screens × 2 panels, wipe path | v3, key storage on 4 platforms | Known/NEW cue, collision warning, optional one-press saving | large; new handshake to re-audit |
| **4. Remembered hosts without C** | as 3, minus grants | as 3 | Known/NEW cue, collision warning | large |
| 5. Time-limited remembering | needs a clock the board lacks | — | — | not feasible without an RTC |
| 6. Per-session only (VK prompt once per session) | = option 1 | — | — | — |
| 7. OS BLE bonding | — | — | a pairing shortcut | ruled out by §0 |

**Under the code-always rule, remembering loses its main benefit.** What is
left is a warning cue (Known / NEW / collision), whose strength depends on how
well each host OS protects the key, and at most one press saved per session.
Against that: a third handshake version to design, implement in C, Rust and
TypeScript, fuzz and audit; a new encrypted store on the wipe path; and key
storage on four host platforms.

**Recommendation:**

1. **Now: option 2.** Finish the per-session viewing-key prompt, make sure
   every host pins the board by seed fingerprint and says so plainly, and
   write the SDK guidance in §5.4. No protocol change.
2. **Fold the cheap part of v3 into the next handshake change whenever one
   happens for another reason:** put the version into the transcript
   (`PROTOCOL.md` §7) and add the R-5/R-7 hygiene. That keeps the door open.
3. **Build option 4 (remembered hosts without the viewing-key skip) only if
   the owner wants the NEW-device cue for hostile environments**, and build C
   only with an explicit yes, given §0's concern. If built, follow §4 and §7
   exactly, and keep Remember devices **Off** by default.

## 7. Implementation plan (if option 3 or 4 is chosen)

Each step is small, lands with its tests, and keeps `./scripts/check.sh`
green. All tests run in the simulator (`sim/`) or against zdev; no hardware
until step 10.

0. **Check assumptions.** Vault key available with zero stored seeds;
   the viewing-key prompt from the other branch merged; its screen code
   reused.
1. **v3 derivation, pure** (`session.c`): `T3`, `k_id`, `se`, `k_conf`.
   Known-answer vectors in `vectors/`, checked by `sim/test_session`, the
   Rust crate and the TS core (all three byte-identical). Re-run
   `sim/passkey_grind.c` against v3, with and without identity.
2. **v3 state machine** (`session.c`, `protocol.c`): `helloConfirm`, one
   confirm per reveal, busy gate on the painting leg, timeouts (R-7),
   `{2,3}` accepted. `sim/test_protocol.c` conformance cases: missing or bad
   MAC, second confirm, swapped `S_h`, stripped identity, small-order `S_h`,
   wrong version. Add the v3 legs to the fuzz corpus (F-1).
3. **Host list store** (`src/hosts.c`, new): encode, encrypt, decrypt,
   lookup, add, replace, forget, limits. Fake-NVS tests: round trip, a
   flipped byte rejected, full list, PIN change re-encryption, power cut
   mid-write leaves the old list.
4. **Wipe path:** `leek_hosts` in `device_wipe()` and its resume; test that
   an interrupted wipe finishes it.
5. **Board screens:** pairing variants, Remember?, Settings > Devices,
   forget confirms. Hints added to `hints.h` and `test_hints.c`; PNG renders
   for both panels from the colour kit's host renderer, checked by eye.
6. **Viewing-key grants** (only if C is approved): scoped match, RAM-only
   rules for temporary, SD and passphrase seeds, the two-second notice.
   Tests for each scope miss.
7. **`leek-device`:** v3, `HostIdentity`, `HostKeyStore`, v2 fallback with
   its message. Tests against zdev in `zecz-core/tests/device_zdev.rs`.
8. **App core (TS), Chrome extension, leek-android, Zodl fork:** v3 and key
   storage per §5.2; mock conformance (`mock-conformance.test.ts`) extended to
   the new legs.
9. **Docs:** `PROTOCOL.md` §3 and §7, the `leek-device` README, SDK guidance.
10. **Audit and hardware:** an `AUDIT-TRANSPORT-3.md` pass over v3, then a
    board test over USB and BLE. Flashing needs the owner's go-ahead.

## 8. Decisions for the owner

1. **Build it at all?** Recommended: option 2 now, remembered hosts later only
   for the NEW-device cue.
2. **Viewing-key skip (C):** allow it, scoped to already-shared accounts and
   off by default, or never?
3. **Remember devices default:** Off (recommended) or On.
4. **List size:** 8 hosts?
5. **Board static key:** none (recommended) until flash encryption or the
   secure element.
6. **`forgetMe` over the protocol:** allow (it can only remove the caller)?
7. **v2 support window:** how many releases the board accepts v2.
8. **Expiry by pairing count:** none, or drop entries unused for N pairings?
9. Any **flashing** for the hardware test in step 10.
