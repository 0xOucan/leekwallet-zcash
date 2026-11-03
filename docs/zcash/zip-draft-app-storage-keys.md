    ZIP: Unassigned
    Title: Hardware-Wallet App Storage Keys
    Owners: 0xOucan
    Status: Draft
    Category: Standards / Wallet
    Created: 2026-11-04
    License: MIT
    Discussions-To: <TODO>
    Pull-Request: <TODO>

<div class="note"></div>

This is a draft kept in the LeekWallet Zcash repository. It has not been
submitted to the ZIP editors, has no ZIP number, and may change in any way.
`ZIP_NUMBER` below stands for the number the ZIP editors would assign.


# Terminology

The key words "MUST", "MUST NOT", "REQUIRED", "SHOULD", "SHOULD NOT",
"RECOMMENDED", "MAY", and "OPTIONAL" in this document are to be interpreted as
described in BCP 14 [^BCP14] when, and only when, they appear in all capitals.

The terms "Mainnet" and "Testnet" are to be interpreted as described in § 3.12
‘Mainnet and Testnet’ of the Zcash Protocol Specification [^protocol-networks].

The terms below are to be interpreted as follows:

Signer

: A device that holds a wallet seed and does not reveal it, such as a hardware
  wallet. It has its own screen and at least one physical input the user
  operates to approve or refuse a request.

App

: Software running on a general-purpose host (phone, desktop computer, browser
  extension) that keeps wallet data for an account whose seed is held by a
  Signer: for example a light wallet that syncs with the chain using the
  account's unified full viewing key (UFVK) [^zip-0316].

App identifier (`app_id`)

: A short ASCII string, in reverse-DNS form, that names an App. Its syntax is
  given in [App identifier](#app-identifier).

Storage Key

: A 32-byte symmetric key, derived by the Signer from the seed as specified in
  this ZIP, that an App uses to encrypt its wallet data at rest.

Storage Subkey

: A 32-byte key derived by the App from a Storage Key for one purpose (for
  example the database, or the stored UFVK).

Authenticated session

: A communication session between a Signer and a host, encrypted with keys
  established by a pairing that the user verified on the Signer's screen (for
  example by comparing or entering a short code shown by the Signer).


# Abstract

This ZIP lets an App that works with a hardware wallet keep its wallet data
encrypted at rest under a per-App Storage Key. The Signer derives the Storage
Key from its seed with ZIP 32 Registered key derivation [^zip-0032-rkd],
from the App identifier, the network (coin type) and the account. The Signer
releases a Storage Key only over an authenticated session and only after
showing the App identifier on its screen and receiving the user's approval.
The App stores only ciphertext and forgets the key when it closes.

Storage Keys for different Apps, networks or accounts are unrelated to each
other and to every spending and viewing key. The same seed always gives the
same Storage Keys, so a user who restores the seed onto a new Signer can open
the data the App stored before.


# Motivation

A light wallet that uses a hardware wallet does not hold spending keys, but it
still holds sensitive data on the host: the UFVK, which reveals every incoming
and outgoing shielded transaction of the account, and a database of notes,
transactions, memos, addresses and contacts derived with it. Today this data
is usually stored in plaintext or protected only by the host's own mechanisms.
Anyone who obtains the host, a disk image or a cloud backup of it, learns the
user's whole transaction history and can keep watching future transactions
with the UFVK.

The user already owns a device built to protect secrets and to ask for
approval on a trusted screen. If that device can hand an App a key that only
it can produce, the App's data at rest becomes as hard to open as the hardware
wallet is to use: the attacker needs the Signer, its PIN, and the user's
approval on it.

The key must be reproducible from the seed alone, so that restoring the seed
backup is enough to recover the App's data, with no new secret to back up.
ZIP 325 [^zip-0325] states the same goal for wallet metadata. It must also be
different for each App, so that one App, or a backup of it, cannot open
another's data.


# Privacy Implications

- A host, a disk image or a backup taken while the App is closed contains only
  ciphertext. Without the Signer and the user's approval it reveals neither
  the UFVK nor the transaction history.
- Storage Keys for different App identifiers, networks or accounts are
  unrelated. Ciphertexts produced under them cannot be linked to each other or
  to the seed by their keys. The [Recommended storage encryption](#recommended-storage-encryption)
  uses a random salt for its key check value so that two encrypted files of
  the same App and account are not linkable by their headers either.
- While the App is open it holds the UFVK and the decrypted data in memory,
  exactly as it would without this ZIP. This ZIP does not protect against a
  malicious App or a compromised host (see
  [Security and privacy considerations](#security-and-privacy-considerations)).
- The Signer shows the App identifier on its screen. Someone who can see the
  screen learns which App the user is opening.


# Requirements

- The seed never leaves the Signer. Releasing a Storage Key reveals nothing
  about the seed or about any spending or viewing key.
- Storage Keys are deterministic: the same seed, App identifier, network and
  account always give the same Storage Key.
- Storage Keys for different App identifiers, networks or accounts are
  computationally independent.
- The derivation is public, so that any conforming Signer, and in an
  emergency an offline tool given the seed phrase, produces the same keys.
- The user sees which App asks for a key, and approves each release on the
  Signer.
- A Storage Key is released only over an authenticated session.
- No new backup is needed beyond the existing seed phrase (and passphrase, if
  any).


# Non-requirements

- Authenticating the App. A Signer cannot tell which software is asking; any
  App can claim any App identifier. The protection rests on the user's
  approval and on the authenticated session.
- Protecting data from the App itself, or from malware running in the App's
  process or with the user's privileges on the host while the App is open.
- Rotating a Storage Key without changing the seed.
- Synchronising or backing up the encrypted data. Where the ciphertext is kept
  is up to the App.
- Storing any of this data on chain.


# Specification

## Key tree

This ZIP registers the following ZIP 32 Registered Key Derivation
[^zip-0032-rkd] tree:

- $\mathsf{ContextString} = \texttt{“AppStorageKeys”}$ (14 bytes, ASCII)
- $\mathsf{ZipNumber} = \mathtt{ZIP\_NUMBER}$

with the structure:

- $m_{\mathsf{storage}}$: App Storage Key tree
    - $m_{\mathsf{storage}} / \mathtt{ZIP\_NUMBER}' / \mathsf{coinType}' / \mathsf{account}'$ - Account Storage Root
        - $\ldots / (0', \mathsf{AppIdTag})$ - App Storage Key (full-width cryptovalue)
        - $\ldots / \ldots$ - (Reserved for future updates to this ZIP)

$S$ is the seed byte sequence from which the account's ZIP 32 [^zip-0032]
shielded keys are derived, meeting the requirements of ZIP 32 "Wallet seeds".
For a wallet created from a BIP 39 [^bip-0039] mnemonic this is the 64-byte
BIP 39 seed, which includes the effect of the BIP 39 passphrase, if any.

$\mathsf{coinType}$ is the SLIP 44 [^slip-0044] coin type used by ZIP 32 for
the network: 133 for Mainnet, and 1 for Testnet and regtest.
$\mathsf{account}$ is the ZIP 32 account index, $0 \leq \mathsf{account} < 2^{31}$.

### Account Storage Root

$\mathsf{AccountStorageRoot} = \mathsf{CKDreg}(\mathsf{CKDreg}(\mathsf{RegKD}(\texttt{“AppStorageKeys”}, S, \mathtt{ZIP\_NUMBER}), \mathsf{coinType} + 2^{31}, [\,]), \mathsf{account} + 2^{31}, [\,])$

or, in path notation:

```
m_storage / ZIP_NUMBER' / coin_type' / account'
```

The Account Storage Root, and every key above it in the tree, MUST NOT leave
the Signer.

### App Storage Key

Let $\mathsf{AppIdTag}$ be the encoding of the App identifier defined in
[App identifier](#app-identifier). Then:

- Let $K = \mathsf{derive\_child\_cryptovalue}(\mathsf{AccountStorageRoot}, 0 + 2^{31}, \mathsf{AppIdTag})$
  (64 bytes, ZIP 32 "Full-width child cryptovalue derivation").
- $\mathsf{StorageKey} = K[0..32]$, the first 32 bytes of $K$.

The remaining 32 bytes of $K$ are discarded and MUST NOT be released.

In terms of the ZIP 32 primitives this is:

- $I = \mathsf{BLAKE2b}\textsf{-}\mathsf{512}(\texttt{“ZIPRegistered\_KD”}, [14]\,||\,\texttt{“AppStorageKeys”}\,||\,[\mathsf{length}(S)]\,||\,S)$; $(\mathsf{sk}, \mathsf{c}) = (I_L, I_R)$.
- For each $i$ in $(\mathtt{ZIP\_NUMBER} + 2^{31},\ \mathsf{coinType} + 2^{31},\ \mathsf{account} + 2^{31})$:
  $(\mathsf{sk}, \mathsf{c}) = \mathsf{PRF^{expand}}(\mathsf{c}, [\mathtt{0xAC}]\,||\,\mathsf{sk}\,||\,\mathsf{I2LEOSP}_{32}(i))$ split into halves.
- $K = \mathsf{PRF^{expand}}(\mathsf{c}, [\mathtt{0xAC}]\,||\,\mathsf{sk}\,||\,\mathsf{I2LEOSP}_{32}(2^{31})\,||\,[\mathtt{0x01}]\,||\,\mathsf{AppIdTag})$.

where $\mathsf{PRF^{expand}}(k, t) = \mathsf{BLAKE2b}\textsf{-}\mathsf{512}(\texttt{“Zcash\_ExpandSeed”}, k\,||\,t)$.
The ZIP 32 definitions are normative; this expansion is given for clarity.
With the `zip32` Rust crate (version 0.3.0) it is
`zip32::registered::cryptovalue_from_subpath(b"AppStorageKeys", seed, ZIP_NUMBER, &[(coin_type', []), (account', []), (0', app_id_tag)])`.

Child indices $1'$ to $\mathtt{0x7FFFFFFF}'$ below the Account Storage Root
are reserved for future updates to this ZIP.

## App identifier

An App identifier is an ASCII string in reverse-DNS form, for example
`xyz.leekwallet.zcash`. It MUST match this ABNF [^rfc5234]:

```
app-id  = label 1*( "." label )
label   = lead *62( lead / "-" / "_" )
lead    = %x61-7A / DIGIT   ; a-z or 0-9
```

and its total length MUST be between 3 and 64 bytes inclusive. Upper-case
letters, spaces, and any non-ASCII character are not allowed; there is no
normalization step. An identifier that does not match is invalid.

- An App SHOULD use a reverse-DNS name under a domain its developer controls,
  so that identifiers of different Apps do not collide by accident. This is a
  naming convention only; nothing verifies it.
- An App MUST use the same identifier on every platform and in every version
  that should open the same data. Different editions of an App that should not
  share data MUST use different identifiers.

The identifier is encoded as a tag for ZIP 32 by prefixing its length:

$\mathsf{AppIdTag} = [\mathsf{length}(\mathsf{app\_id})]\,||\,\mathsf{app\_id}$

where $\mathsf{length}$ is the number of bytes and fits in one byte.

## Signer requirements

A Signer implementing this ZIP:

1. MUST derive Storage Keys exactly as specified in [Key tree](#key-tree), and
   MUST NOT release anything else from the tree: not the Account Storage Root,
   not any key above it, and not the second half of $K$.
2. MUST check that the requested App identifier is valid, and MUST refuse an
   invalid one rather than correct or normalize it.
3. MUST, before releasing a Storage Key, show on its own screen:
   the App identifier, in full and exactly as received (scrolling if it does
   not fit, never truncated); the network; and the account index. It SHOULD
   also say in words that approving lets this App open its stored wallet data.
4. MUST require an explicit approval by the user on the Signer for every
   release. It MUST NOT approve by timeout, by default, or from a decision
   remembered from an earlier request or session. Refusing MUST be possible
   at every step.
5. MUST derive the Storage Key only after approval, from the same App
   identifier bytes it showed. It MUST NOT release a Storage Key for an App
   identifier, network or account the user was not shown.
6. MUST release a Storage Key for at most one App identifier per approval.
7. MUST release a Storage Key only inside an authenticated session. It MUST
   NOT release it over an unauthenticated or unencrypted channel, and MUST NOT
   display it, for example as a QR code.
8. SHOULD refuse requests for a network other than the one it is set to
   serve.
9. MAY release the account's UFVK and the Storage Key under one approval
   (the first connection of an App), provided the screen names both.

## App requirements

An App implementing this ZIP:

1. MUST request Storage Keys only for its own App identifier.
2. MUST NOT write a Storage Key, a Storage Subkey, the UFVK, or decrypted
   wallet data to non-volatile storage in plaintext, except as allowed by
   [Remembered mode](#remembered-mode).
3. MUST NOT send a Storage Key or Storage Subkey to any server (for example a
   lightwalletd server or a backup service).
4. SHOULD erase the Storage Key, Storage Subkeys, the UFVK and decrypted data
   from memory when it closes or locks, as far as its platform allows.
5. MUST NOT use the Storage Key directly as a cipher key. It MUST derive a
   Storage Subkey per purpose, as in [Storage Subkeys](#storage-subkeys).

### Usage flow (non-normative)

- First connection: the App and the Signer pair with user verification. The
  user approves on the Signer, which releases the UFVK and the Storage Key.
  The App creates its wallet database encrypted under the "db" Storage
  Subkey, stores the UFVK encrypted under the "ufvk" Storage Subkey, and
  forgets the keys when it closes.
- Later openings: the App pairs again and requests only the Storage Key. The
  user approves on the Signer. The App decrypts the UFVK and the database
  locally, then syncs.
- New Signer or new host: restoring the same seed (and passphrase) on any
  conforming Signer gives the same Storage Key, so a copy of the ciphertext
  on any host opens again.

### Remembered mode

An App MAY offer the user a choice between:

- "Don't remember" (the default this ZIP is written for): every opening
  needs the Signer and the user's approval, as above.
- "Remember the viewing key on this device": the App keeps the Storage Key,
  or the UFVK, in the platform's secure storage (for example a key held by
  Android Keystore or the Apple Keychain), and its database stays encrypted
  at rest. Opening the App then does not need the Signer, and the protection
  of the data falls back to the host's own lock screen and secure storage.

An App that offers remembered mode MUST make it an explicit choice of the
user, MUST explain the difference, and MUST let the user switch back, which
deletes what was remembered.

## Storage Subkeys

A Storage Subkey for the purpose $\mathsf{label}$ is:

$\mathsf{SubKey}(\mathsf{label}) = \mathsf{HKDF}\textsf{-}\mathsf{SHA256}(\mathsf{IKM} = \mathsf{StorageKey},\ \mathsf{salt} = [\,],\ \mathsf{info} = \texttt{“Zcash\_AppStorage\_v1”}\,||\,[\mathsf{length}(\mathsf{label})]\,||\,\mathsf{label},\ \mathsf{L} = 32)$

using HKDF [^rfc5869] with SHA-256, an empty salt, and $\mathsf{label}$ an
ASCII string of 1 to 64 bytes. This ZIP defines the labels:

| Label | Purpose |
|---|---|
| `db` | Encryption of the App's wallet database |
| `ufvk` | Encryption of the stored UFVK |
| `commit` | Key check value (see below) |

Labels beginning with `x-` are for an App's private use. All other labels are
reserved for future updates to this ZIP.

## Recommended storage encryption

Apps SHOULD encrypt each stored object with an AEAD under the Storage Subkey
for its purpose:

- XChaCha20-Poly1305 [^xchacha] with a random 192-bit nonce per encryption
  is RECOMMENDED.
- AES-256-GCM [^nist-gcm] with a random 96-bit nonce MAY be used where
  XChaCha20-Poly1305 is not available (for example the Web Crypto API). An
  App using it MUST NOT encrypt more than $2^{32}$ messages under one
  Storage Subkey.
- A database MAY instead use a page-level encryption scheme (for example
  SQLCipher) keyed with the raw `db` Storage Subkey.

Neither AEAD above commits to its key. To detect a wrong key (a different
seed, network, account or App identifier) before decrypting, and to avoid
attacks that rely on a ciphertext decrypting under more than one key, an
encrypted object SHOULD carry a key check value in its header:

- $\mathsf{salt}$: 32 random bytes, chosen per object.
- $\mathsf{check} = \mathsf{HMAC}\textsf{-}\mathsf{SHA256}(\mathsf{SubKey}(\texttt{“commit”}), \mathsf{salt})$.

The App MUST compare $\mathsf{check}$ in constant time and refuse to decrypt
if it differs. The random salt keeps the check value from being a stable
identifier of the account.

The header (format version, algorithm, salt, check value, nonce) and the
context (App identifier, coin type, account and label) SHOULD be passed to
the AEAD as associated data, so that a ciphertext cannot be moved to another
context or have its header changed undetected.

## Emergency recovery (non-normative)

Because the derivation is public and deterministic, an offline tool given the
seed phrase (and passphrase) can compute a Storage Key without a Signer, for
example if the Signer is lost and no replacement is at hand. Doing this
exposes the seed to a general-purpose computer and defeats the purpose of the
hardware wallet. Such a tool SHOULD warn the user, SHOULD run only offline,
and a seed exposed this way SHOULD be treated as no longer secret.


# Rationale

## Why Registered key derivation

ZIP 32 Registered key derivation gives a key tree tied to a ZIP number and a
context string, so it cannot collide with the tree of any other ZIP or with
the Sapling, Orchard or transparent trees. Ad-hoc key derivation is
deprecated by ZIP 32 because it gives no such guarantee. Registered derivation
also allows tags on child indices, which this ZIP uses to carry the App
identifier, and a full-width cryptovalue at a leaf.

The Storage Key is taken from the full-width cryptovalue (lead byte 1) rather
than from the secret key of a tree node. ZIP 32 recommends this for leaf
outputs: the cryptovalue cannot be used to derive further nodes, so a
released Storage Key cannot be confused with, or extended into, other
keys of the tree. Taking the first 32 bytes of a pseudorandom 64-byte
output is enough for a 256-bit symmetric key.

The path follows the ZIP 32 account structure (coin type, then account), as
ZIP 325 does, so that Storage Keys separate by network and account the same
way spending and viewing keys do.

## Why per App

If every App shared one key, approving one App would let it open the data of
every other App, and a leaked backup of one App's key would expose all of
them. With a key per App identifier, the user's approval on the Signer grants
exactly what the screen says.

## Why not ZIP 325 Account Metadata Keys

ZIP 325 metadata keys solve a similar problem for software wallets, but are
account-wide: the Account-level Inherent Metadata Key covers every metadata
protocol of the account, including all private-use subtrees. A Signer that
released it to one App would give that App the keys of every other App.
A private-use subject under ZIP 325 would avoid that, but its derivation is
not coordinated by any ZIP. A separate registered tree keeps Storage Keys
independent of whatever is later standardised under ZIP 325. Defining this
protocol instead as a standardised metadata protocol under ZIP 325 is listed
under [Open issues](#open-issues).

## Why not derive from the viewing key

The UFVK is one of the things the App stores encrypted, so a key derived from
it would be circular. It would also let anyone the user gave the UFVK to (an
auditor, for example) open all the App's stored data.

## Why not signatures over App-chosen messages

Some wallets derive encryption keys by hashing a signature over a fixed
message. Zcash shielded signatures (RedJubjub, RedPallas) are randomized, so
they do not give a reproducible key. Using a deterministic transparent
signature would tie storage encryption to a spending key, and asking a Signer
to sign App-chosen messages with a spending key is a dangerous capability by
itself: the message could be a transaction sighash. A dedicated derivation
needs no spending authority and cannot be confused with a signature.

## Why not on-chain storage

An encrypted UFVK in a memo would have to be found and decrypted with the
viewing key, which is circular. Data on chain is also permanent: if the key
ever leaked, the data could not be withdrawn.

## Why ASCII reverse-DNS identifiers

The identifier is the only thing the user sees when deciding whether to
approve, often on a small screen with a limited font. ASCII lower case avoids
look-alike characters from other scripts and any question of Unicode
normalization: the bytes shown are the bytes derived from. Refusing invalid
identifiers instead of normalizing them means one displayed string always
corresponds to one key. Reverse-DNS names give a familiar namespace that
avoids accidental collisions. The 64-byte limit keeps the identifier readable
on a hardware wallet screen.

## Why a length-prefixed tag

ZIP 32 recommends a prefix-free encoding for tags. A length byte makes the
encoding prefix-free and leaves room to add fields in a future version.


# Security and privacy considerations

App impersonation
: Any software on the host can claim any App identifier. A malicious App that
  claims another App's identifier receives that App's Storage Key if the user
  approves. To use it, it also needs the other App's ciphertext: on platforms
  that sandbox App data (Android, iOS) it usually cannot read it; on desktop
  systems software running as the same user usually can. Signers SHOULD
  phrase the approval screen so that the user approves only right after
  opening that App themselves.

Compromised host or malicious App
: While open, the App holds the Storage Key, the UFVK and the decrypted data.
  Malware with access to the App's memory gets all of them. This ZIP protects
  a host that is lost, stolen or imaged while the App is closed, and backups
  of it; it does not protect against the App itself.

Lost Signer, seed exposure, flash readout
: Storage Keys are derived from the seed, so anyone who has the seed, or can
  read it out of the Signer's storage, can compute every Storage Key. This
  adds no exposure beyond what the seed already allows (spending the funds).
  Users who lose the Signer recover the data by restoring the seed on a new
  one.

BIP 39 passphrase
: A different passphrase gives a different seed and therefore different
  Storage Keys. The data of a passphrase-protected account opens only with the
  same passphrase.

Rotation
: A Storage Key is fixed for a given seed, network, account and App
  identifier. If the host was compromised while the App was open, the UFVK
  and the data were exposed, and a new Storage Key would not undo that: the
  remedy is a new seed, moving the funds to it, and re-encrypting under the
  new seed's Storage Key. Rotation within one seed is not provided (see
  [Open issues](#open-issues)).

Cross-App linkability
: Storage Keys of different App identifiers, networks and accounts are
  unrelated, and the recommended header uses a random salt for its check
  value, so ciphertexts do not link to each other or to the seed through the
  keys. The App identifier, if stored in a header, does reveal which App made
  the file.

Nonce reuse
: AEAD nonces are random. XChaCha20-Poly1305's 192-bit nonce makes collisions
  negligible; with AES-256-GCM the $2^{32}$-message limit above applies.

Channel
: The Storage Key is released only inside an authenticated session, so a
  nearby attacker who relays or spoofs a Bluetooth link without the user
  verifying the pairing does not obtain it.

Remembered mode
: In remembered mode the data is as safe as the host's secure storage and
  lock screen, and no longer requires the Signer. Apps make this an explicit
  user choice.


# Deployment

This ZIP needs no consensus change. Signers and Apps adopt it independently;
an App works with any Signer that implements it. Before public deployment the
ZIP number must be assigned, since it is an input to the derivation and every
Storage Key changes with it.


# Reference implementation

To be written in the LeekWallet Zcash project
(<https://github.com/0xOucan/leekwallet-zcash>):

- Signer: the LeekWallet firmware (ESP32-S3), as a new device call that shows
  the App identifier, network and account, and releases the Storage Key over
  the existing paired session.
- Apps: the `leek-zcash` host SDK (`host/leek-zcash`), used by the LeekWallet
  Zcash desktop and Android app and the Chrome extension.

Both will use the `zip32` Rust crate, version 0.3.0 or later
(`zip32::registered`).


# Test vectors

<div class="warning"></div>

PROVISIONAL. These vectors use $\mathtt{ZIP\_NUMBER} = 65535$ as a placeholder
and are not valid for any deployment. They will be regenerated from the
reference implementation once a ZIP number is assigned.

They were computed with the `zip32` crate 0.3.0
(`registered::cryptovalue_from_subpath`) and checked against an independent
implementation of the ZIP 32 formulas. The seed is the test seed
`000102…1f` (32 bytes, 0x00 to 0x1f), never a real seed.
$\mathsf{ContextString} = \texttt{“AppStorageKeys”}$.

Vector 1: Mainnet, account 0, `xyz.leekwallet.zcash`

```
coin_type    = 133
account      = 0
app_id_tag   = 1478797a2e6c65656b77616c6c65742e7a63617368
K            = f43c678ee392cc4552daa3bde21c3cb15b416811dfdacf64e7aceca6114bd0d0
               65a7e0e28f8c0f992b29877519e7c03565f49f0a87818969a8dd95a8dcb384de
storage_key  = f43c678ee392cc4552daa3bde21c3cb15b416811dfdacf64e7aceca6114bd0d0
subkey db    = abd9811ecdfd468b538e7f11714bdacca99e354ea622d31f0ee3b2cee3f85973
subkey ufvk  = 8946f802ebbfdef22df61d9ce06f675bc04e7d2a4f5ceae1bd3d56be5646cc3f
subkey commit= 7af26c9ac8d79de53e6e565ca1eb26e249ff35685f4d2758aebfc97b1cac51dd
```

Vector 2: Testnet, account 0, `xyz.leekwallet.zcash`

```
coin_type    = 1
account      = 0
app_id_tag   = 1478797a2e6c65656b77616c6c65742e7a63617368
storage_key  = 854cfb1ecebae4b636bc37c1043370b16e658a79fd5402098ffd7d7ddea277fd
subkey db    = b676000d114d159c7ab7b1a235987a06f467b9b8285833dab1e316eb41b7ff1a
subkey ufvk  = c48632e7b9153c11e0ff7a7336747b96594998e94cb91ff9fc58951e304c2e1c
subkey commit= 18bd65966b6b16adff3c4222ab7a22ba12b6c6fb3041f41ff0001de003801bdc
```

Vector 3: Mainnet, account 1, `xyz.leekwallet.zcash`

```
coin_type    = 133
account      = 1
app_id_tag   = 1478797a2e6c65656b77616c6c65742e7a63617368
storage_key  = 26efbcd704c2229a500441fafc913394bd8b576a96593b77096aaf6fa2c894a7
subkey db    = 19bf70dc5659a1b0d6bc4be96662a5505ac6fabd13ce57c9c168a38efa376f59
subkey ufvk  = a248cfd3c6541d3f54abc7661fa2cb5f96ad793cc8e070a7e3cdaee70f8ee302
subkey commit= c62953d52efc2e8bf1bc6427d28d0ff6206e5f9a99409cbd85a0493daa4c80b2
```

Vector 4: Mainnet, account 0, `com.example.wallet`

```
coin_type    = 133
account      = 0
app_id_tag   = 12636f6d2e6578616d706c652e77616c6c6574
storage_key  = 3977f965125080aed7b50e09ebbfc3b1760732e0b24ac8bb039514c36587d944
subkey db    = 80eb7f02d41fc666d0dc279d44f7d73539b4af0c8e9883e084bdd11f595913fe
subkey ufvk  = 3f6e8cd26fe5549a7c67bda380679357f8dd37a096febe67d9f6288c8f290c36
subkey commit= 398a63cebbe64f2a782cf3d339b73c5b56351d974add6fa9da135f0b3f0e4a61
```

Final vectors should also be added to zcash-test-vectors and include invalid
App identifiers that a Signer must refuse (upper case, non-ASCII, a single
label, empty labels, over 64 bytes).


# Open issues

- TODO (owner): add a contact email to the Owners header; ZIP 0 requires
  one for each owner.
- Whether to submit this to zcash/zips as a separate ZIP, or as a
  standardised metadata protocol under ZIP 325 (a 2000-series update).
- The context string `“AppStorageKeys”` and the HKDF info prefix
  `“Zcash_AppStorage_v1”` are proposals.
- Rotation within one seed (for example a generation index in the path) was
  left out because the user would have to remember the generation to restore.
- Whether a Signer should be allowed to release keys for an account of the
  other network than the one it is set to.
- A standard wire format for the request and response between Signer and App
  is out of scope here; it could be added if more than one Signer adopts this.


# References

[^BCP14]: [Information on BCP 14 — "RFC 2119: Key words for use in RFCs to Indicate Requirement Levels" and "RFC 8174: Ambiguity of Uppercase vs Lowercase in RFC 2119 Key Words"](https://www.rfc-editor.org/info/bcp14)

[^protocol-networks]: [Zcash Protocol Specification, Version 2025.6.3 [NU6.1]. Section 3.12: Mainnet and Testnet](https://zips.z.cash/protocol/protocol.pdf#networks)

[^zip-0032]: [ZIP 32: Shielded Hierarchical Deterministic Wallets](https://zips.z.cash/zip-0032)

[^zip-0032-rkd]: [ZIP 32: Shielded Hierarchical Deterministic Wallets, Section: Registered key derivation](https://zips.z.cash/zip-0032#specification-registered-key-derivation)

[^zip-0316]: [ZIP 316: Unified Addresses and Unified Viewing Keys](https://zips.z.cash/zip-0316)

[^zip-0325]: [ZIP 325: Account Metadata Keys](https://zips.z.cash/zip-0325)

[^bip-0039]: [BIP 39: Mnemonic code for generating deterministic keys](https://github.com/bitcoin/bips/blob/master/bip-0039.mediawiki)

[^slip-0044]: [SLIP 44: Registered coin types for BIP-0044](https://github.com/satoshilabs/slips/blob/master/slip-0044.md)

[^rfc5234]: [RFC 5234: Augmented BNF for Syntax Specifications: ABNF](https://www.rfc-editor.org/rfc/rfc5234)

[^rfc5869]: [RFC 5869: HMAC-based Extract-and-Expand Key Derivation Function (HKDF)](https://www.rfc-editor.org/rfc/rfc5869)

[^xchacha]: [XChaCha: eXtended-nonce ChaCha and AEAD_XChaCha20_Poly1305 (IRTF CFRG draft)](https://datatracker.ietf.org/doc/html/draft-irtf-cfrg-xchacha)

[^nist-gcm]: [NIST SP 800-38D: Recommendation for Block Cipher Modes of Operation: Galois/Counter Mode (GCM) and GMAC](https://csrc.nist.gov/pubs/sp/800/38/d/final)
