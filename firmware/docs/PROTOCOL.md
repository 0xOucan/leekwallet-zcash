# LeekWallet Zcash Wire Protocol

One command set over two transports (BLE GATT, USB CDC-ACM). The firmware dispatcher and the
`@leekwallet/core` TypeScript client are both written against this document, so it needs to be
settled before either side starts.

---

## 1. Threat model — read this before designing anything on top

Three distinct attackers, and the protocol only defends against two of them.

| Attacker | Defended by | Effective? |
|---|---|---|
| Passive radio eavesdropper | Session encryption (§3) | Yes |
| Active MITM between host and device | Commit-then-reveal passkey comparison (§3) | Yes — one online guess at 1-in-10⁶, and only if the user is looking |
| **Compromised host application** | **Nothing in this protocol** | **No** |

The third one is the important one. If the phone or PC is compromised, the attacker is *inside*
the app, above the encryption layer. They see every keystroke before it is encrypted and can
substitute any payload they like.

So the protocol's job is not to make the host trustworthy. It is to ensure that **anything the
host cannot be trusted with is decided on the device, in front of the user's eyes.** That gives
one hard rule which the whole command set is organised around:

> **Rule 1.** Every operation that moves funds or reveals a secret requires physical
> confirmation on the device, against data rendered by the device from its own parsed state —
> never from a string supplied by the host.

The host is a keyboard and a screen. It is never an authority.

---

## 2. Framing

```
┌────────┬────────┬──────────────────────┐
│ len:u16│ type:u8│ payload (CBOR)       │
└────────┴────────┴──────────────────────┘
   big-endian, len covers type + payload
```

CBOR over JSON: compact enough for a 244-byte BLE MTU, binary-safe for hashes and signatures,
and it has a strict canonical form — which matters, because a permissive parser on a signing
device is an attack surface.

| type | meaning |
|---|---|
| `0x01` | Request (plaintext) |
| `0x02` | Response (plaintext) |
| `0x11` | Request (encrypted) |
| `0x12` | Response (encrypted) |
| `0x7E` | Error (encrypted) |
| `0x7F` | Error (plaintext) |

Plaintext is **not** "pre-session only", and saying so was wrong in a way that
mattered: the device answers a plaintext request in plaintext even while a
session is up, which is the counter rule in section 6 and not a special case.
What plaintext does not get is *authority* — every tier above `getStatus`
refuses it with `0x0400` whether or not a session exists.

There is no event type. `0x13 Event` was in this table and never in the
firmware; nothing is unsolicited, and `0x7E` — which the firmware does send,
and which section 6 relies on — was missing. See `src/protocol.c:55-60`.

**BLE chunking.** GATT writes cap at MTU−3. Frames are split into chunks with a 1-byte header:
bit 7 = "more follows", bits 0-6 = sequence, wrapping at 128. The receiver reassembles before
parsing. USB CDC uses the same frames without chunking. Everything above this layer is
transport-blind. Chunking is the *only* thing BLE adds; a transport that altered anything else
would be a second protocol.

The device chunks to the MTU it actually negotiated (`ble_att_mtu()` on the connection handle),
never an assumed one, and logs that MTU at connect. Chunking smaller than the link allows costs
packets; chunking larger is silently truncated by the stack and reaches the peer as a *corrupt*
frame rather than a short one. Hosts that cannot read the negotiated MTU (btleplug exposes no
accessor on any backend) should assume the 23-byte floor and chunk at 19 payload bytes, which is
always safe.

**The sync marker is USB-only.** `src/protocol.c` emits and requires `'L','K'` before every frame
because that port is shared with console log output, so a receiver has to be able to find a frame
in a stream that also carries text. A GATT characteristic carries nothing but our frames and
already delimits every write, so **BLE frames start at `len:u16` with no marker** — which is also
what `framing.ts` encodes, being transport-blind. Two bytes is over 10% of a 19-byte payload
budget at MTU 23, so the cost is not nominal either. Concretely:

| Transport | On the wire |
|---|---|
| USB-Serial-JTAG | `'L' 'K'` ‖ len:u16 ‖ type ‖ payload |
| BLE GATT | chunk header ‖ (len:u16 ‖ type ‖ payload, split across chunks) |

The Rust serial transport adds and strips the marker; the BLE client must not. A mismatch here
presents as a corrupt-frame bug rather than a convention disagreement, which is why it is
tabulated rather than described.

**GATT layout** (ROADMAP T25):

| | UUID | Properties |
|---|---|---|
| Service | `6c65656b-7761-6c6c-6574-000000000001` | — |
| Host → device | `6c65656b-7761-6c6c-6574-000000000002` | Write, Write Without Response |
| Device → host | `6c65656b-7761-6c6c-6574-000000000003` | Notify |

**Limits.** Max frame 4 KB (`PROTOCOL_MAX_FRAME`, 4096 bytes), which is what a
`zSignChunk` of 3072 PCZT bytes needs with the CBOR around it. The rejection happens on the
declared length, before the bytes are buffered — a signing device must never let the peer
dictate an allocation size. On BLE that check runs on the first chunk, and a chunk that is out
of sequence, empty, overshoots the declared length or falls short of it resets reassembly
rather than being patched around.

BLE carries the same limit rather than a matching one: `BLE_CHUNK_MAX_FRAME` is *defined as*
`PROTOCOL_MAX_FRAME`. They used to be two numbers under a comment claiming they agreed, and
when one moved the other stayed behind, so a frame that went over the cable could not be sent
over BLE at all. There is no second number to forget now.

---

## 3. Session establishment

Runs once per connection, before any encrypted command.

```
host                                                device
 ── 0x01 hello { version: 2, hostPubkey: PKa } ───────▶
                            device picks Nb, keeps it, publishes only its hash
 ◀── 0x02 { version: 2, devicePubkey: PKb,
            deviceCommit: Cb = H("…commit-v2" ‖ PKb ‖ PKa ‖ Nb) }
 ── 0x01 helloReveal { hostNonce: Na } ───────────────▶
 ◀── 0x02 { deviceNonce: Nb }
      host checks Cb, and aborts the connection if it does not open
      both: T  = H("…transcript-v2" ‖ PKa ‖ PKb ‖ Na ‖ Nb)
            ss = X25519(a, PKb) = X25519(b, PKa)
            k_h2d, k_d2h, passkey = HKDF(salt = T, ikm = ss, info = label)
 ◀── device displays passkey = be32(HKDF(…"passkey-v2")[0..3]) mod 10^6
 ── user compares the two screens and presses ALLOW on the device
 ── 0x11 encrypted traffic ───────────────────────────▶
```

**Two round trips, and the ordering is the security property.** Version 1 of this protocol had
one, no nonces and no commitment: the passkey was `HKDF(X25519(a,B), "…passkey-v1")[0..3] mod
10^6`, a deterministic function of the shared secret alone. A relay knows the host's public key,
so it could compute what the host *would* display for any private key it chose and search
offline until that matched the six digits the device was already showing. Nothing crossed the
wire while it searched and no attempt failed. `sim/passkey_grind.c --v1 --search` still does it,
against trezor-crypto's deliberately slow reference X25519 — **425 364 derivations in 91 s on one
core** when `docs/AUDIT-TRANSPORT.md` §4 measured it, and single-digit seconds for an optimised
multicore implementation. The wall-clock figure is machine-dependent and has been quoted as three
different numbers in this repository; the rate is the durable part, and it is around 5 000
derivations per second per core against the slowest X25519 in the tree.

What closes it is the mechanism BLE Secure Connections and ZRTP use, adopted rather than
approximated:

- Bluetooth Core Specification v5.4, Vol 3, Part H, §2.3.5.6.4 (Numeric Comparison): the
  non-initiator sends `Cb = f4(PKbx, PKax, Nb, 0)` before the initiator reveals `Na`, and the six
  digits are `g2(PKax, PKbx, Na, Nb) mod 10^6` — over both public keys and both nonces.
- RFC 6189 (ZRTP) §4.4.1.1: "A hash commitment precludes this attack by forcing the MiTM to
  choose his own two DH public values before learning the public values of either of the two
  parties." Its SAS is derived from the total hash of the transcript (§4.5.2).

The device is the non-initiator here, so the device commits. Work through what that leaves a
relay running two handshakes at once. On the **device-facing** leg it must send its public key
and then its nonce before the real device reveals `Nb` — and the digits the OLED will show
depend on `Nb`. On the **host-facing** leg it must send its commitment before the real host
reveals `Na` — and the digits the app will show depend on `Na`. Every input it controls is fixed
before the input that decides the answer arrives. There is no offline phase left: it can only
pick and hope, once, at 1-in-10⁶, and a wrong guess is two screens that disagree in front of a
user who was asked to compare them.

Two consequences that are not optional:

- **The host must verify `deviceCommit` against the revealed `deviceNonce` and refuse to pair if
  it does not open.** Skipping that check restores the v1 attack exactly, because an
  unverified nonce is one the peer may choose after seeing everything else.
- **The device must answer at most one `helloReveal` per `hello`.** A second reveal is a second
  derivation over a nonce the peer has already seen.

Both keys are bound to the transcript too — it is the HKDF-Extract salt — so substituting a
public key or a nonce anywhere changes every derived value, not only the digits.
`sim/passkey_grind.c` (default mode) re-runs the relay against this construction; the search
that succeeded against v1 finds nothing it can use — 0 matches in 20 000 derivations even when
the host's nonce is handed to it — and committed attempts land where 1-in-10⁶ says they should.

**What is still true:** none of this authenticates *which* device you are talking to. There is
no long-term key and no attestation. It proves that the two ends of this connection are talking
directly to each other and to nobody in between, and it proves it only if the user actually
compares the digits.

Payload encryption is **ChaCha20-Poly1305** with a per-direction 96-bit nonce that is a
monotonic counter. Counters never reset within a session, and a frame that replays or reorders
fails authentication. **A repeat is not otherwise detected:** the counter is 32 bits inside a
96-bit nonce (`src/session.c:305-315`, `session.ts:195`) and both ends wrap silently past
`0xFFFFFFFF` rather than aborting. Unreachable in practice — 2^32 frames is weeks of unbroken
traffic — but it is arithmetic doing the work, not a check, and this line used to claim
otherwise. The
ESP32-S3's AES accelerator would make AES-GCM tempting, but ChaCha20 is constant-time in
software everywhere, which matters more on the host side than raw throughput does at our sizes.

**What this buys and what it does not:** an eavesdropper learns nothing. A MITM is detected, with
the odds and the caveat above. A compromised host is entirely unaffected — see §1.

### A handshake does not interrupt a question the device is asking

`hello` is plaintext and unauthenticated by construction — it runs before there is anything to
authenticate with — and it tears the current session down. Left ungated, that let anyone able to
write to the port or the characteristic choose the moment a signing approval vanished from the
screen, taking the request with it. So the device **defers a handshake while a confirmation is
on screen**: a signing approval, a host-proposed passphrase (either kind), a wipe, or a seed on
display or being verified. The host gets `0x0401` ("the device is waiting for the user; retry")
and retries. The exact list is `ui_user_is_answering()` in `src/ui.c`.

**Both legs are gated, and the second one is the one that matters.** `hello` deliberately shows
nothing — the passkey does not exist until the reveal — so gating only `hello` gated the half
that never paints. A peer could open a handshake at a quiet moment, sit in `AWAITING_REVEAL`
indefinitely (nothing times it out, and USB has no disconnect), and spend its `helloReveal` at a
moment of its own choosing to wipe a seed off the glass. `helloReveal` now defers on the same
condition, *before* consuming the commitment, so the deferral costs the host a retry rather than
a fresh handshake.

What is **not** gated, and is a deliberate gap rather than an oversight: `SCREEN_MNEMONIC_ENTRY`
reached from Import Wallet, where the user is typing a recovery phrase with the link still live.
Wallet *creation* is covered by something stronger — `screen_entropy_enter()` calls
`transport_suspend()`, so nothing is listening for the whole of it — but the import path has no
equivalent, and a peer can therefore clear a half-typed phrase (`forget_mnemonic_entry`) at will.
It destroys work rather than leaking anything, but it is the same class as the seed-on-display
case that *is* listed.

Deliberately a deferral and not a lockout, and deliberately *not* "refuse whenever a session is
active": USB has no disconnect for the device to notice, so an app that crashed and restarted
would otherwise be shut out until someone power-cycled the wallet. The pairing screen itself is
also not gated — a second handshake there resets and repaints, so the user compares the new
digits rather than being stranded behind a screen only a button can clear.

---

## 3b. One transport at a time

**The device exposes exactly one channel at a time: USB or BLE, never both.**
Selectable on the device, defaulting to USB.

This is not a preference. Three reasons, in increasing order of how badly they
bite:

1. **The session layer is single-peer by construction.** There is one session
   state and one pair of nonce counters (`src/session.c`). Two concurrent peers
   would advance the same counters and each other's frames would fail to
   decrypt — the same class of fault that a request colliding with a
   background poll already produced on a single channel.
2. **The framing has no request IDs.** A reply belongs to whichever request
   went out last. Two channels means two "last"s.
3. **A device listening on BLE while plugged into USB is reachable by someone
   you cannot see.** The user believes they are on a cable. Advertising should
   be off — not merely unpaired — whenever BLE is not the selected transport,
   which is also the honest answer to the privacy question in T56: a wallet
   that does not advertise announces nothing.

### The advertised name (T56)

Whatever the device broadcasts is read by everyone in range, so the owner can
change it — on the device, in Settings → BLE Name, and nowhere else. There is
deliberately no protocol method for renaming: a host that could rename the
device could make it advertise as something else entirely, and the name is one
of the few things a user can check against their own phone.

The name lives in the **scan response**, not the advertisement. Both together do
not fit: a legacy advertisement is 31 bytes, 3 go to flags and 18 to the
complete 128-bit service UUID, and "LeekWallet" needs 12 more than remain.
NimBLE then rejects the whole set of fields and advertising never starts, which
on a battery-powered device looks exactly like a device that is working. It
shipped that way once.

A user-supplied name is therefore bounded at **29 bytes** — 31 minus the 2-byte
AD header — and one that does not fit is **refused**, never truncated:
truncation would advertise a device the user never named, and they are the only
one in a position to notice. The same check is applied to what comes back out of
storage, since that could have been written by another firmware version.

Switching transports tears down any active session. There is no state worth
carrying across, and pretending otherwise would mean deciding whether a passkey
confirmed over one channel authorises the other. It does not.

## 4. Commands

Permission tiers mirror the existing `RPC_PERM_*` model in the pixiecolibri sibling project.

**Read the Status column before building a client.** This table was written as
a design for the whole surface, and five of its rows have never been
implemented — a client written from the unqualified table would call methods the
device answers as unknown. The authority on what the firmware speaks is the
`strcmp(method, ...)` chain in `src/protocol.c`; this column is kept honest
against it.

| Command | Tier | Device confirmation | Status |
|---|---|---|---|
| `ping` | always | — | implemented |
| `getFeatures` | always | — | implemented |
| `getStatus` | always | — | implemented |
| `unlock` | always | PIN entered **on device** | implemented |
| `lock` | always | — | implemented |
| `listWallets` | unlocked | — | **not implemented** |
| `selectWallet` | unlocked | — | implemented |
| `setPassphrase` | unlocked | **yes — fingerprint confirm (§5)** | implemented |
| `clearPassphrase` | unlocked | — | **not implemented** — `setPassphrase` with an empty string is refused, and locking is the way back to the base wallet |
| `getFingerprint` | keys | — | **not implemented** — the fingerprint is shown on the device's own screen |
| `getAddress`, `signMessage`, `signTypedData`, `signTransaction` | — | — | **removed** in the Zcash fork (roadmap 1.1): an unknown method |
| `zGetAccount` | keys | — (viewing key, not spend authority) | implemented |
| `zGetFeatures` | always | — (describes the firmware, not the wallet) | implemented |
| `zSetAccountMeta` | keys | — (birthday and label; not secret, not needed to sign) | implemented |
| `zShowAddress` | keys | **yes — the user compares the address and answers on the device** | implemented |
| `zSignBegin` / `zSignChunk` / `zSignFinish` | keys | **yes — every review page, ACCEPT on the last** | implemented |

Plus `hello` and `helloReveal`, which carry the handshake (§3) rather than a
command.

There is no `signHash`: the board never signs a digest the host hands it. It
parses the PCZT, computes the sighash itself, shows what it computed and signs
only that.

An unknown method is an error frame, never silence, and identical over both
transports — `sim/test_protocol.c` runs every conformance case down each channel
and compares the replies.

**Deliberately absent:** there is no host-invokable `wipe`, no `getMnemonic`, and no way to set
or change the PIN over the wire. Those are device-only, permanently. A protocol that can erase
your wallet is a protocol that a malicious host can erase your wallet with.

`unlock` does not carry a PIN. It asks the device to prompt; the user types on the device; the
response says whether it worked. The PIN never crosses the wire in any form.

### getStatus

Readable before a session, in plaintext, and polled by the companions:

```
getStatus → { account, activeWallet, network, temporary, passphrase, unlocked, walletCount }
```

| field | |
|---|---|
| `account` | the ZIP 32 account the board's own screens are on |
| `activeWallet` | 1-based stored wallet in use; 0 when none, and for the whole of temporary mode |
| `network` | `"main"` or `"test"`: the board's Settings → Network (below). Absent on firmware before it was added |
| `temporary` | 1 while signing from a seed held in RAM only |
| `passphrase` | 1 while a passphrase is applied (never which one) |
| `unlocked` | 1 once the PIN was entered on the board |
| `walletCount` | stored wallets |

`getFeatures` describes the firmware (`chain: "zcash"`, `firmware`, `model`)
and `zGetFeatures` the signer (below); neither says anything about the wallet.

### The board's network

The board answers for one Zcash network: the one its owner chose under
Settings → Network on the board (persisted; Mainnet out of the box). Every
request that names a `network` (`zGetAccount`, `zShowAddress`,
`zSetAccountMeta`, `zSignBegin`) is refused when it names the other one,
before anything is derived, with error `0x0528` and a message naming both:

```
0x7E { code: 0x0528,
       message: "the board is set to Testnet; switch it to Mainnet in Settings > Network, or switch the app to Testnet" }
```

The board says so on its screen too ("Wrong network", the app's network and
its own), for four seconds or until a button. `zSignFinish` checks again,
because the setting can be switched while a PCZT uploads; it then drops the
upload. `getStatus` reports the setting as `network`, so a companion can say which
side to switch, or follow the board, before it asks; it follows a switch at
once. A companion turns `0x0528` into a sentence of its own naming the
setting to change (leek-device's `wrong_network_message`, the extension's
`wrongNetworkMessage`). A board on Testnet once handed a mainnet app the
mainnet account without a word; a user who believes the board is on the
other network can send real funds believing them play money.

### Zcash account metadata

```
zGetAccount     { account?: uint, network: "main"|"test" }
             -> { account, network, seedFingerprint: h'32', ufvk, ua, tAddress, birthday? }
zSetAccountMeta { account?: uint, network?: "main"|"test", birthday?: uint, label?: text }
             -> { stored: 0|1 }
```

`birthday` is present in `zGetAccount` only when the device has one for that
account and network of the open seed (records are per network: the same
account is born at a far lower height on mainnet; `network` defaults to
`"test"` in `zSetAccountMeta` for companions that predate it, all of them
testnet-only, and not to the board's setting, which would file a testnet
birthday under mainnet; on a board set to Mainnet a request without
`network` is refused with `0x0528` like any testnet one): a companion set it with `zSetAccountMeta`, or the
seed came from an SD `.zkv` file that carried it. A companion should use it
instead of asking the user, and set it after an import when it was absent.
Fields left out of `zSetAccountMeta` keep their values. `label` is at most 32
printable ASCII characters.

`stored: 1` means the record is in flash, keyed by the seed's short
fingerprint and the account. That happens only for a stored wallet with no
BIP39 passphrase. For a passphrase, temporary or SD seed the answer is
`stored: 0`: the record is held in RAM and dropped on lock, so flash never
shows that such a seed was used.

### Zcash features and address check

```
zGetFeatures  {}
           -> { backend: "both"|"rust"|"c", branchIds: [uint], txVersions: [6],
                spendPools: ["orchard", "ironwood", "transparent"], maxPczt: uint, maxChunk: uint,
                storage: ["vault","stateless","sd"?], showAddress: 1 }
zShowAddress  { account?: uint, network: "main"|"test" }
           -> { ua, tAddress }
```

A companion reads `zGetFeatures` before building a transaction. It can do so
before pairing: it describes the firmware and says nothing about the wallet.
`branchIds` is the list both signer backends accept. `test_zcash_sign` checks
it against what they actually accept.

`zShowAddress` has the device derive the account's address itself and page
the whole unified address on its screen. The user answers on the last page
whether it matches what the app shows. The address comes back only after a
yes. A no is `0x0200` (rejected) and no answer is `0x0201` (timeout).
`zecz device-show-address` runs the check and also compares the address the
device confirmed with the one its own copy of the UFVK gives.

### Zcash signing (PCZT)

A Zcash transaction reaches the device as a PCZT (ZIP 374), which the
companion builds and the device checks. Three calls, all encrypted, all
requiring an unlocked device:

```
zSignBegin  { len: uint, sha256: h'32', account: uint, network: "main"|"test" }
         -> { maxChunk: 3072, backend: "both"|"rust"|"c" }
zSignChunk  { offset: uint, data: h'1..maxChunk' }      offset = bytes so far
         -> { received: uint }
zSignFinish {}
         -> { sighash: h'32',
              signatures: [ { pool: 1|2, index: uint, sig: h'64' }
                          | { pool: 0, index: uint, sig: h'9..73', pubkey: h'33' } ] }
```

`zSignFinish` refuses unless every byte arrived and they hash to `sha256`,
then checks the PCZT with both signer backends (`src/zcash-sign.c`). In BOTH
mode, the default, their summaries must be byte-identical, sighash included.
The device then shows the review pages (`src/zcash-review.c`) and waits for
the user. Only ACCEPT on the last page signs; every signature is verified by
both backends before it leaves. The reply carries the sighash the user
reviewed, so the host can check it against its own. A rejection, timeout or
lock drops the upload. Pools: 0 transparent, 1 Orchard, 2 Ironwood
(`ZS_POOL_*`).

A transparent entry (roadmap 10.1) carries the input index, the DER
signature followed by the sighash type byte (always 0x01, `SIGHASH_ALL`),
and the compressed pubkey it verifies under: the same fields as a version 2
`.zsig` entry (docs/zcash/FORMATS.md, which also lists the checks the device
makes on each input before it shows it as "from your t-address"). `sighash`
stays the shielded sighash; each transparent signature commits to its own
input's ZIP 244 digest, which the host recomputes when it applies it.

A refusal is error `0x05xx`, where the low byte is the reason: `-ZS_E_*`
from the signers (`zsign_types.h`: 0x0A parse, 0x0B branch, 0x0C foreign
spend, 0x0D output commitment, 0x0E user address, 0x0F value balance,
0x10 unsupported bundle, 0x11 network, ...) or `-ZSIGN_E_*` from the
intake (`zcash-sign.h`: 0x1E out of order, 0x1F size, 0x20 offset, 0x21
hash, 0x22 incomplete, 0x23 backends disagree, ...). 0x11 is a PCZT for
another network than the request names; 0x28 (`ZSIGN_E_BOARD_NETWORK`) is a
request for another network than the board's setting, and is not only
zSign's (above, "The board's network").

---

## 5. Passphrase entry — the "app as secure keyboard"

Typing a passphrase with a knob (or four buttons) is punishing, so the app can act as the keyboard. This is
worth doing, and it is what Trezor Suite does. But the security accounting has to be honest.

```
setPassphrase { passphrase: "..." }        (encrypted, §3)
  ▼
device derives seed, computes fingerprint + first address
  ▼
device displays:   seed fingerprint 3A7B1C22
  ▼
user confirms on the device → passphrase becomes active for the session
```

**What this protects against:** the radio, and a MITM. Not the host. A compromised app sees the
passphrase as it is typed, before encryption touches it. Host entry is therefore *strictly
weaker* than on-device entry, and the UI must say so rather than implying the encryption makes
it equivalent.

That said, the tradeoff is reasonable for the stated use case — a cold-storage device used
occasionally from a machine you control — provided three things hold:

1. **The app uses its own in-app keyboard for this field, never the system IME.** This is not
   optional on Android. Third-party keyboards (Gboard, SwiftKey) sync typed text to the cloud
   and keep learned-word caches. A passphrase typed into a system IME should be considered
   disclosed. Disable autocorrect, clipboard, and screenshots on that view.
2. **The passphrase is never persisted host-side.** Not in local storage, not in a "remember
   this wallet" convenience, not in a crash log.
3. **The fingerprint is confirmed on the device screen.** Which brings us to the real problem.

### Why the fingerprint check matters, and its one limitation

A mistyped passphrase does not error. It derives a different, perfectly valid, empty wallet.
Users conclude their funds are gone. So the device shows the seed fingerprint before the passphrase is used for
anything, and `setPassphrase` answers with the same number.

Be precise about what that check catches:

- **On every use after the first:** it catches both typos *and* a malicious host substituting a
  passphrase, because you recognise your own fingerprint. This is a genuine tamper detector.
- **On first use:** it catches nothing, because you have no reference to compare against.

Therefore: **record the fingerprint when the passphrase is created**, on the device screen, and
write it down with the seed backup. Without that reference the check is decorative on the one
occasion it would matter most. Coldcard's XFP-on-screen convention exists for exactly this
reason and we should follow it.

Derivation is plain BIP39 — `PBKDF2-HMAC-SHA512(mnemonic, "mnemonic" + passphrase, 2048)` — and
is verified against the spec's known-answer vectors in `sim/test_passphrase.c`. Any seed and
passphrase produce the same BIP39 seed here as in any other BIP39 wallet.

### What the companion app must do about it

The device drops the passphrase on lock, wallet switch and disconnect. The host
has to mirror that, and the reason is sharper than tidiness.

Addresses derived under a passphrase belong to a wallet the device can no longer
produce once that passphrase is gone. Left on screen they look exactly like
valid ones. So:

- **Derived state is valid only for the exact `(unlocked, wallet, passphrase)`
  tuple it was derived under.** Any change discards it, including a fresh
  unlock — the user may have entered a different passphrase, or none.
- **The app must poll**, not merely react to its own commands. The device
  auto-locks on its own timer and the user can switch wallets by hand; neither
  passes through the app. `getStatus` returns `unlocked`, `activeWallet`, `account`,
  a boolean `passphrase` and a boolean `temporary`, which is enough to detect
  every case.
- **`activeWallet` is 0 for the whole of temporary mode**, because no stored
  seed is selected while a RAM-only one is in use. A host reading only that
  number cannot tell "signing from a seed this device will never keep" from
  "something is wrong", and the app rendered `wallet 0/5` — which reads as a
  fault. `temporary` is what distinguishes them. It is not a secret: it says a
  seed is in RAM, never what it is, and the same fact is on the device's own
  screen throughout.
- **Never persist derived addresses.** A passphrase wallet leaves no trace on
  the device by design, and writing its addresses into host storage undoes
  precisely that: anyone reading the app's data learns a hidden wallet exists,
  which is the fact being protected. Memory only.

Note the status flag says *whether* a passphrase is applied, never which one,
and it is session state rather than something stored — so it reveals nothing
about a device sitting locked.

### Session lifetime

The passphrase lives in RAM only and is cleared on: device lock, PIN re-entry, wallet switch,
transport disconnect, and inactivity timeout. The status bar must always show which wallet is
active, because "am I in the passphrase wallet or the base wallet?" is the question users get
wrong and lose money over.

---

## 6. Errors

```
0x7F { code: u16, message: "short ascii" }
```

| code | meaning |
|---|---|
| `0x0001` | Malformed frame |
| `0x0002` | Unsupported version |
| `0x0100` | Not unlocked |
| `0x0101` | Wrong permission tier |
| `0x0200` | User rejected on device |
| `0x0201` | Timed out waiting for the user |
| `0x0300` | No wallet selected |
| `0x0400` | Session required, or the channel is unusable (a frame that failed to authenticate) |
| `0x0401` | Transport busy; the request was refused, not acted on — resend it |
| `0x05xx` | The device will not sign this PCZT; the low byte is the reason (Zcash signing, above) |
| `0x0528` | The request names the other Zcash network than the board's Settings → Network; nothing was derived (section 4, "The board's network") |

### A reply matches the frame type of the request that caused it

An **encrypted** reply (`0x12` / `0x7E`) is only ever produced for a request the
device successfully **decrypted**. A plaintext request gets a plaintext reply,
even while a session is up.

This is a counter rule, not a style rule. The device advances its receive
counter the moment a frame decrypts, and the host advances its send counter
only when it opens a reply, so an error answering an encrypted request *must*
be encrypted or the two drift apart permanently. The mirror holds just as
firmly: a plaintext request moved neither counter, and answering it with
ciphertext the host has no key for leaves the device→host stream one ahead
forever — as well as handing a host with no session a body it can only try to
parse as CBOR. (That is exactly what happened on hardware: a host that had lost
its session sent a plaintext request and reported "unsupported CBOR major type
7".)

A plaintext frame still proves nothing about who sent it, so every permission
tier above `getStatus` refuses it with `0x0400` regardless of whether a session
exists.

### Every complete request is answered

No transport may reply to a complete, well-formed frame with silence. A host
cannot tell a dropped request from a slow one, and this protocol has no request
IDs to resynchronise with, so a lost reply pairs every later reply with the
wrong request. Where a transport has a queue between reception and dispatch —
BLE does, because a signing request blocks on a human and the NimBLE host task
cannot be the thread that waits — a full queue is refused with `0x0401` in
plaintext rather than dropped.

Messages are for developers. Never render a device-supplied string to the user as if it were a
security statement — that is a phishing vector.

---

## 6b. Session model: unlock once, confirm every time

Ledger's flow is unlock with the PIN, then sign repeatedly without re-entering
it, pressing to approve each transaction. We match that, deliberately:

| | Required |
|---|---|
| Unlocking the device | PIN, on the device |
| Each signature | **Physical confirmation, no PIN** |
| After auto-lock | PIN again |

Asking for a PIN per signature sounds stricter and is not. It trains PIN entry
into muscle memory, which is the habit that makes shoulder-surfing and fake
prompts work. The per-signature control is the button press against
device-rendered data; the PIN establishes the session.

## 6c. The mock, checked against the firmware

`sim/test_protocol.c` runs the real `src/protocol.c` on the host, which is what
makes "the mock must never be more permissive than the device" checkable
instead of aspirational. The comparison is made by machine:

    make -C sim conformance     # protocol.c answers the corpus, byte for byte
    pnpm --dir app test         # the mock answers the same bytes; diff or die

`sim/test_protocol.c --emit-vectors` replays a fixed corpus through the real
dispatch and records the request and the plaintext reply as hex.
`app/packages/core/test/mock-conformance.test.ts` feeds the *same bytes* to
`MockDevice` and compares frame type, map shape, every field name and every
value, except build identity and free-text error `message`s. Error `code`s are
compared exactly.

What the corpus settled, each time in the firmware's favour:

| Firmware | Mock, before | Why the firmware is right |
|---|---|---|
| `unlock` answers `{prompted:1, unlocked:0}` and the host polls `getStatus` | unlocked synchronously | An app built on the mock believed unlocking is instant |
| unknown method with no session → `0x0001` | `0x0400` "no session" | `dispatch()` matches the name first and only the handlers it found check the tier. Answering "pair with me first" for `getMnemonic` tells a stranger a method exists that does not |
| `getFeatures` has no `initialized`; `lock` answers `{unlocked:0}` | both differed | A field present in the mock and absent from the firmware is a field app code comes to depend on |
| `selectWallet` with no `index` → `0x0001` | `{activeWallet:1}` | Defaulting made a host that dropped the field look correct here and select nothing there |
| `setPassphrase` validates length and charset → `0x0001` | applied anything, including `""` | A passphrase enterable from the app but not from the device's own keyboard is a wallet the owner cannot reach without the app |

The shapes:

| Command | Request | Reply |
|---|---|---|
| `selectWallet` | `{ index: 1-based }` | `{ activeWallet }`, `0x0300` if there is no such wallet; drops the passphrase |
| `setPassphrase` | `{ passphrase: printable ASCII }` | `{ fingerprint, passphrase: 1 }` after the on-device confirm |

`hello` is the one exchange the corpus cannot compare: the device answers with
an X25519 public key and the mock has no key agreement at all. The test replays
it only for its side effect, a session for the next request to travel inside.
The z* methods are compared against `zdev` (the firmware's protocol on a PTY)
rather than the mock.

## 7. Versioning

**The current version is 2.** `hello` carries it in both directions, and **both ends check**:

| Situation | What happens |
|---|---|
| Host offers a version the device does not speak (including v1, which sent no `version` field at all) | Device answers `0x0002` in plaintext, naming both versions, and derives nothing |
| Device answers a version the host does not speak | Host aborts before deriving, naming both versions |
| Versions agree | The handshake continues |

Mismatch is a hard failure with an upgrade prompt, not a best-effort downgrade: silent
negotiation to a weaker protocol is a downgrade attack, and v1 is a protocol whose passkey
comparison an attacker can defeat offline (§3).

This was aspirational until v2 shipped, and the gap was the interesting part. v1's `hello`
already carried a version — and nothing ever read it. A mismatched pair therefore got as far as
deriving keys from labels the other end had never used, and failed on the first encrypted frame
with "decrypt failed", which is true and tells the user nothing they can act on. The check is
worth no more than the error message it produces, so both ends name both versions.

The domain-separation labels carry the version too (`leek-session-h2d-v2` and its siblings), so
even if the explicit check were somehow bypassed the two ends could not accidentally agree on a
key. That is belt and braces, not the mechanism: the mechanism is the check, because only the
check can produce a sentence a user can act on.

**The version is not in the transcript, and that is load-bearing only while there is one of
them.** `T` hashes `PKa ‖ PKb ‖ Na ‖ Nb` and nothing else, so the offered and answered version
numbers are bound *indirectly*, by the `-v2` in the KDF labels. Today that is airtight: exactly
one version is accepted at each end, so there is no version a relay could substitute that both
ends would still derive under. It stops being airtight the moment a v3 accepts `{2, 3}` — a relay
could then offer v2 to a v3 device and v3 to a v2 host, and nothing inside `T` would record the
downgrade. Whoever adds a third version must put the negotiated version into the transcript hash
in the same change, not after it.

The same is true, more mildly, of the messages themselves. `T` binds the four cryptographic
values, not the bytes that carried them: unknown CBOR keys in `hello` or `helloReveal` are
ignored rather than hashed, so the transcript is a summary of the handshake and not a transcript
of it in ZRTP's sense (RFC 6189 §4.5.2 hashes the actual messages). Nothing in v2 reads a field
that is outside `T`, so there is nothing to substitute — but that is an invariant to preserve,
and it is one an added field would quietly break.

---

## Open questions

- **Does the passkey confirm survive BLE re-pairing**, or is it re-run per connection? Per
  connection is safer and costs the user one screen glance.
- `zGetAccount` hands the viewing key to any paired host without a press on the board. It is
  not spend authority, but it is the whole history; whether to ask once per session is open.
